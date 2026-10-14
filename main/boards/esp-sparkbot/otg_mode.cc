#include "otg_mode.h"

#include <esp_log.h>
#include <lvgl.h>
#include <nvs_flash.h>
#include <cstring>

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
    ShowOverlay(PanelState::Audio);
    SetUsbStatus(StatusText());
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
    for (;;) {
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
        const char* notice = nullptr;
        if (events & kToggle) {
            notice = faulted_ ? "USB 异常，请复位" : SwitchMode();
            switching_ = false;
        }
        // Reconcile current state, not a stale mount event from before handoff.
        bool connected = !native_ && !faulted_ && usb.IsInitialized() && usb.IsConnected();
        usb_connected_ = connected;
        if (faulted_) {
            active_ = false;
            ShowOverlay(PanelState::Error);
            SetUsbStatus("USB 异常");
        } else if (connected) {
            Enter();
        } else {
            active_ = false;
            HideOverlay();
            SetUsbStatus(native_ ? "USB 调试" : "");
        }
        // SetStatus hides notifications, so present results AFTER reconciliation.
        if (notice && display_) display_->ShowNotification(notice, 3500);
    }
#endif
}

const char* OtgMode::SwitchMode() {
#if CONFIG_USB_AUDIO_DEVICE
    auto& usb = UsbAudio::GetInstance();
    active_ = false;
    usb_connected_ = false;
    ShowOverlay(native_ ? PanelState::ToAudio : PanelState::ToNative);
    SetUsbStatus("USB 切换中");

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
        if (!SaveMode(native_)) return "切换成功，保存失败";
        return native_ ? "长按 BOOT 返回音频" : "等待电脑识别音频";
    }
    return faulted_ ? "USB 异常，请复位" : "失败，已保留原模式";
#else
    return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// 屏幕显示
// ---------------------------------------------------------------------------

void OtgMode::SetUsbStatus(const char* text) {
    if (!display_ || (status_text_ && std::strcmp(status_text_, text) == 0)) return;
    status_text_ = text;
    display_->SetStatus(text);
}

void OtgMode::ShowOverlay(PanelState state) {
    if (!display_) return;
    DisplayLockGuard lock(display_);
    if (overlay_ && panel_state_ == state) return;

    auto* overlay = static_cast<lv_obj_t*>(overlay_);
    if (!overlay) {
        auto* screen = lv_scr_act();
        if (!screen) return;
        overlay = lv_obj_create(screen);
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
        // A single column keeps all labels in flow; the old absolute layout
        // placed the title and body at the same origin on the 240x240 display.
        lv_obj_set_size(overlay, LV_PCT(90), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(overlay, 12, 0);
        lv_obj_set_style_pad_row(overlay, 8, 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(overlay, lv_color_hex(0x111C2A), 0);
        lv_obj_set_style_radius(overlay, 14, 0);
        lv_obj_set_style_border_width(overlay, 1, 0);
        lv_obj_set_style_shadow_width(overlay, 16, 0);
        lv_obj_set_style_shadow_opa(overlay, LV_OPA_20, 0);
        lv_obj_set_style_shadow_color(overlay, lv_color_hex(0x000000), 0);

        auto make_label = [overlay](uint32_t color) {
            auto* label = lv_label_create(overlay);
            lv_obj_set_width(label, LV_PCT(100));
            lv_obj_set_height(label, LV_SIZE_CONTENT);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_line_space(label, 3, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
            return label;
        };
        badge_ = make_label(0x38BDF8);
        title_ = make_label(0xF8FAFC);
        detail_ = make_label(0xCBD5E1);
        hint_ = make_label(0x94A3B8);
        overlay_ = overlay;
        // A screen rebuild must not leave stale LVGL pointers in the manager.
        lv_obj_add_event_cb(overlay, [](lv_event_t* event) {
            auto* self = static_cast<OtgMode*>(lv_event_get_user_data(event));
            self->overlay_ = self->badge_ = self->title_ = nullptr;
            self->detail_ = self->hint_ = nullptr;
            self->panel_state_ = PanelState::Hidden;
        }, LV_EVENT_DELETE, this);
    }

    const char* badge = "USB AUDIO";
    const char* title = "音箱 + 麦克风";
    const char* detail = "PC 音频由本机播放\n麦克风同步传回 PC";
    const char* hint = "双键切换调试\n长按 BOOT 5 秒切换";
    uint32_t accent = 0x38BDF8;
    if (state == PanelState::ToAudio || state == PanelState::ToNative) {
        badge = "SWITCHING";
        title = state == PanelState::ToNative ? "切换到调试模式" : "切换到音频模式";
        detail = "正在交接 USB 接口\n电脑将自动重新识别";
        hint = "无需拔线或重启\n切换中，请稍候";
        accent = 0xFBBF24;
    } else if (state == PanelState::Error) {
        badge = "USB ERROR";
        title = "USB 暂不可用";
        detail = "USB 初始化失败\n已停止切换以防冲突";
        hint = "按 RESET 复位设备\n或断电后重新上电";
        accent = 0xFB7185;
    }
    lv_label_set_text(static_cast<lv_obj_t*>(badge_), badge);
    lv_label_set_text(static_cast<lv_obj_t*>(title_), title);
    lv_label_set_text(static_cast<lv_obj_t*>(detail_), detail);
    lv_label_set_text(static_cast<lv_obj_t*>(hint_), hint);
    lv_obj_set_style_text_color(static_cast<lv_obj_t*>(badge_), lv_color_hex(accent), 0);
    lv_obj_set_style_border_color(overlay, lv_color_hex(accent), 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(overlay);
    lv_obj_move_foreground(overlay);
    panel_state_ = state;
}

void OtgMode::HideOverlay() {
    if (!display_) return;
    DisplayLockGuard lock(display_);
    if (overlay_) lv_obj_add_flag(static_cast<lv_obj_t*>(overlay_), LV_OBJ_FLAG_HIDDEN);
    panel_state_ = PanelState::Hidden;
}
