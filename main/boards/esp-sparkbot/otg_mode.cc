#include "otg_mode.h"

#include <esp_log.h>
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
            SetUsbStatus("USB 异常");
        } else if (connected) {
            Enter();
        } else {
            active_ = false;
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
    SetUsbStatus(native_ ? "切换到音频模式" : "切换到调试模式");

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
