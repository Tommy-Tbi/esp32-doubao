#include "otg_mode.h"

#include <esp_log.h>
#include <lvgl.h>
#include <nvs_flash.h>

#if CONFIG_USB_AUDIO_DEVICE
#include "usb_audio.h"
#include "esp_private/usb_phy.h"
#include "esp_private/periph_ctrl.h"
#include "hal/usb_serial_jtag_ll.h"
#endif

#define TAG "OtgMode"

namespace {
constexpr uint32_t kConnectionChanged = 1;
constexpr uint32_t kToggle = 2;

#if CONFIG_USB_AUDIO_DEVICE
// Owned only by the mode worker. TinyUSB owns its own OTG PHY handle.
usb_phy_handle_t native_phy = nullptr;

esp_err_t InstallNative() {
    PERIPH_RCC_ATOMIC() {
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
    usb_serial_jtag_ll_phy_enable_pad(false);
    const usb_phy_config_t config = {
        .controller = USB_PHY_CTRL_SERIAL_JTAG,
        .target = USB_PHY_TARGET_INT,
        // Leave otg_mode/speed at DEFAULT/UNDEFINED for the non-OTG controller.
    };
    esp_err_t err = usb_new_phy(&config, &native_phy);
    if (err != ESP_OK) return err;
    // Explicit software detach, including when booting in native mode.
    usb_serial_jtag_ll_phy_enable_pad(false);
    vTaskDelay(pdMS_TO_TICKS(250));
    usb_serial_jtag_ll_phy_disable_pull_override();
    usb_serial_jtag_ll_phy_enable_pad(true);
    return ESP_OK;
}

esp_err_t UninstallNative() {
    if (!native_phy) return ESP_ERR_INVALID_STATE;
    usb_serial_jtag_ll_phy_enable_pad(false);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_err_t err = usb_del_phy(native_phy);
    if (err == ESP_OK) native_phy = nullptr;
    else usb_serial_jtag_ll_phy_enable_pad(true);
    return err;
}

bool BootInNativeMode() {
    nvs_handle_t h;
    if (nvs_open("otg", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t disabled = 0;
    esp_err_t err = nvs_get_u8(h, "disabled", &disabled);
    nvs_close(h);
    return err == ESP_OK && disabled == 1;
}

bool SaveMode(bool native) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("otg", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "disabled", native ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "Save USB boot mode: %s", esp_err_to_name(err));
    return err == ESP_OK;
}
#endif
}

OtgMode& OtgMode::GetInstance() {
    static OtgMode instance;
    return instance;
}

void OtgMode::Begin(Display* display) {
    display_ = display;
#if CONFIG_USB_AUDIO_DEVICE
    if (task_) return;
    if (xTaskCreatePinnedToCore([](void* arg) { static_cast<OtgMode*>(arg)->Run(); },
                    "usb_mode", 6144, this, 4, &task_, 0) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create USB mode worker");
        return;
    }
    UsbAudio::GetInstance().AddConnectionCallback([this](bool connected) {
        OnUsbConnection(connected);
    });
    OnUsbConnection(UsbAudio::GetInstance().IsConnected());
#else
    ESP_LOGI(TAG, "USB audio disabled, hot switching unavailable");
#endif
}

void OtgMode::ToggleByKeys(uint32_t key_mask) {
#if CONFIG_USB_AUDIO_DEVICE
    bool expected = false;
    if (!task_ || !switching_.compare_exchange_strong(expected, true)) return;
    ESP_LOGI(TAG, "combo 0x%08x: request USB PHY handoff", (unsigned)key_mask);
    xTaskNotify(task_, kToggle, eSetBits);
#else
    (void)key_mask;
#endif
}

void OtgMode::OnUsbConnection(bool connected) {
    // Never perform display work or uninstall inside TinyUSB's own task.
    (void)connected;
    if (task_) xTaskNotify(task_, kConnectionChanged, eSetBits);
}

void OtgMode::Enter() {
#if CONFIG_USB_AUDIO_DEVICE
    active_ = true;
    UsbAudio::GetInstance().Start();
    ShowOverlay();
    if (display_) display_->SetStatus(StatusText());
#endif
}

void OtgMode::Run() {
#if CONFIG_USB_AUDIO_DEVICE
    auto& usb = UsbAudio::GetInstance();
    if (BootInNativeMode()) {
        esp_err_t err = InstallNative();
        native_ = err == ESP_OK;
        faulted_ = err != ESP_OK;
        if (faulted_) ESP_LOGE(TAG, "Native PHY init: %s", esp_err_to_name(err));
    } else {
        // Detach any native interface left enabled by the ROM/bootloader before
        // the very first UAC2 enumeration as well as during runtime handoffs.
        PERIPH_RCC_ATOMIC() {
            usb_serial_jtag_ll_enable_bus_clock(true);
        }
        usb_serial_jtag_ll_phy_enable_pad(false);
        vTaskDelay(pdMS_TO_TICKS(250));
        faulted_ = !usb.Resume();
        if (faulted_) ESP_LOGE(TAG, "Initial USB audio install failed");
    }
    if (faulted_ && display_) display_->ShowNotification("USB 初始化失败，请复位设备", 3000);
    for (;;) {
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
        if (events & kToggle) {
            if (!faulted_) SwitchMode();
            else if (display_) display_->ShowNotification("USB 状态异常，请复位设备", 3000);
            switching_ = false;
        }
        // Reconcile current state, not a stale mount event from before handoff.
        bool connected = !native_ && !faulted_ && usb.IsInitialized() && usb.IsConnected();
        usb_connected_ = connected;
        if (connected) {
            Enter();
        } else {
            active_ = false;
            HideOverlay();
            if (display_) display_->SetStatus(native_ ? "USB Serial/JTAG" : "");
        }
    }
#endif
}

void OtgMode::SwitchMode() {
#if CONFIG_USB_AUDIO_DEVICE
    auto& usb = UsbAudio::GetInstance();
    active_ = false;
    usb_connected_ = false;
    HideOverlay();
    if (display_) display_->ShowNotification("正在切换 USB，请等待电脑重新识别…", 3000);

    bool success = false;
    if (!native_) {
        ESP_LOGI(TAG, "Handoff: UAC2 OTG -> native USB Serial/JTAG");
        if (!usb.Uninstall()) {
            // Do not reuse a PHY whose task/interrupt teardown failed.
            faulted_ = true;
        } else {
            esp_err_t err = InstallNative();
            if (err == ESP_OK) {
                native_ = true;
                success = true;
            } else {
                ESP_LOGE(TAG, "Native PHY install: %s; restoring UAC2", esp_err_to_name(err));
                faulted_ = !usb.Resume();
            }
        }
    } else {
        ESP_LOGI(TAG, "Handoff: native USB Serial/JTAG -> UAC2 OTG");
        esp_err_t err = UninstallNative();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Native PHY release: %s", esp_err_to_name(err));
        } else if (usb.Resume()) {
            native_ = false;
            success = true;
        } else {
            // esp_tinyusb 1.x install errors may leave a partial stack/PHY.
            // Never blindly acquire a second PHY or reboot into the same loop.
            faulted_ = true;
            native_ = false;
        }
    }
    if (success) {
        bool saved = SaveMode(native_);
        if (display_) display_->ShowNotification(
            !saved ? "USB 已切换，但启动模式保存失败" :
            native_ ? "已切到原生 Serial/JTAG，再按组合键恢复音频" :
                      "USB 音频已启用，等待电脑识别", 3000);
    } else if (display_) {
        display_->ShowNotification(faulted_ ? "USB 切换失败，请复位设备" :
                                              "USB 切换失败，已保留原模式", 3000);
    }
#endif
}

// ---------------------------------------------------------------------------
// 屏幕显示
// ---------------------------------------------------------------------------

void OtgMode::ShowOverlay() {
    if (display_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(display_);

    lv_obj_t* overlay = static_cast<lv_obj_t*>(overlay_);
    if (overlay == nullptr) {
        lv_obj_t* screen = lv_scr_act();
        if (screen == nullptr) {
            return;
        }
        overlay = lv_obj_create(screen);
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(overlay, LV_PCT(92), LV_SIZE_CONTENT);
        lv_obj_center(overlay);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(overlay, lv_color_hex(0x101418), 0);
        lv_obj_set_style_radius(overlay, 10, 0);
        lv_obj_set_style_border_width(overlay, 2, 0);
        lv_obj_set_style_border_color(overlay, lv_color_hex(0x00C2FF), 0);
        lv_obj_set_style_pad_all(overlay, 8, 0);

        lv_obj_t* title = lv_label_create(overlay);
        lv_obj_set_width(title, LV_PCT(100));
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0x00C2FF), 0);
        lv_label_set_text(title, "OTG 模式运行中");

        lv_obj_t* desc = lv_label_create(overlay);
        lv_obj_set_width(desc, LV_PCT(100));
        lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(desc, lv_color_hex(0xE0E0E0), 0);
        lv_label_set_text(desc,
                          "USB 音箱 + 麦克风已开启\nPC 音频由本机喇叭播放\n同时按两个键切到 Serial/JTAG");

        overlay_ = overlay;
    }

    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
}

void OtgMode::HideOverlay() {
    if (display_ == nullptr || overlay_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_add_flag(static_cast<lv_obj_t*>(overlay_), LV_OBJ_FLAG_HIDDEN);
}
