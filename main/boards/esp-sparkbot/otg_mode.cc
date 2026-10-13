#include "otg_mode.h"

#include <esp_log.h>
#include <esp_system.h>
#include <lvgl.h>
#include <nvs_flash.h>

#if CONFIG_USB_AUDIO_DEVICE
#include "usb_audio.h"
#include "usb_cdc.h"
#endif

#define TAG "OtgMode"

OtgMode& OtgMode::GetInstance() {
    static OtgMode instance;
    return instance;
}

void OtgMode::Begin(Display* display) {
    display_ = display;

#if CONFIG_USB_AUDIO_DEVICE
    // 追加监听者（不覆盖 AudioService 已注册的回调）
    UsbAudio::GetInstance().AddConnectionCallback([](bool connected) {
        OtgMode::GetInstance().OnUsbConnection(connected);
    });

    if (UsbAudio::GetInstance().IsConnected()) {
        // 理论上不会发生（Begin 在 USB 枚举前调用），兜底处理
        OnUsbConnection(true);
    }
#else
    ESP_LOGI(TAG, "USB audio disabled, OTG mode inactive");
#endif
}

#if CONFIG_USB_AUDIO_DEVICE

void OtgMode::OnUsbConnection(bool connected) {
    ESP_LOGI(TAG, "usb %s", connected ? "connected" : "disconnected");
    usb_connected_.store(connected);

    if (!connected) {
        // 拔出 USB：清掉“按键退出”的锁定，重新插拔后可再次进入
        bool was_active = active_.exchange(false);
        blocked_.store(false);
        if (was_active) {
            Leave(false, 0);
        }
        HideOverlay();
        return;
    }

    if (blocked_.load()) {
        ESP_LOGI(TAG, "OTG mode stays off (exited by keys), unplug to re-enter");
        return;
    }
    Enter();
}

void OtgMode::Enter() {
    active_.store(true);
    UsbAudio::GetInstance().Start();
    ESP_LOGI(TAG, "enter OTG mode (USB speaker + mic)");

    ShowOverlay();
    if (display_) {
        display_->SetStatus(StatusText());
        display_->ShowNotification("OTG 模式已开启，同时按两个按键可退出", 3000);
    }
}

void OtgMode::ExitByKeys(uint32_t key_mask) {
    ESP_LOGI(TAG, "combo 0x%08x -> exit OTG mode", key_mask);

    if (!usb_connected_.load()) {
        if (display_) {
            display_->ShowNotification("当前没有连接 USB，未进入 OTG 模式", 2000);
        }
        return;
    }

    if (!active_.load()) {
        // 之前已经用按键退出过了
        if (display_) {
            display_->ShowNotification("OTG 模式已关闭，重新插拔 USB 可恢复", 2000);
        }
        return;
    }

    Leave(true, key_mask);
}

void OtgMode::Leave(bool by_keys, uint32_t key_mask) {
    (void)key_mask;
    active_.store(false);
    if (by_keys) {
        // 停止 USB 音频通路：麦克风停止上传，PC 音频不再从本地喇叭播出
        blocked_.store(true);
        UsbAudio::GetInstance().Stop();
        ESP_LOGI(TAG, "exit OTG mode by keys");
    }

    HideOverlay();
    if (display_) {
        display_->SetStatus("");
        if (by_keys) {
            display_->ShowNotification("已退出 OTG 模式，切换到 JTAG 模式…", 3000);
        }
    }

    if (by_keys) {
        // 运行时切换：卸载 UAC2 描述符，安装 CDC-only 描述符
        // PC 会重新枚举，先看到 UAC2 消失，然后看到 CDC ACM 出现
        ESP_LOGW(TAG, "Switching USB: UAC2 -> CDC (JTAG console)");
        UsbAudio::GetInstance().Uninstall();
        // 短暂延时，让 PC 完拔 UAC2 设备
        for (volatile int i = 0; i < 10000000; i++) { asm volatile("nop"); }
        if (!UsbCdcInstall()) {
            ESP_LOGE(TAG, "UsbCdcInstall failed");
            if (display_) display_->ShowNotification("切换失败，重启中…", 2000);
            for (volatile int i = 0; i < 10000000; i++) { asm volatile("nop"); }
            esp_restart();
        } else {
            // 标记下次启动仍处于“OTG 禁用”状态，防止一旦重启又加载 UAC2
            nvs_handle_t h;
            if (nvs_open("otg", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "disabled", 1);
                nvs_commit(h);
                nvs_close(h);
            }
            UsbCdcPrint("ESP-SparkBot: OTG exited, now in CDC/JTAG mode.\r\n");
        }
    }
}

void OtgDisableAndRestart() {
    ESP_LOGW(TAG, "Persisting otg/disabled=1 and restarting…");
    nvs_handle_t h;
    if (nvs_open("otg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "disabled", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    // 给屏幕时间刷新通知
    for (volatile int i = 0; i < 20000000; i++) { asm volatile("nop"); }
    esp_restart();
}

#else  // CONFIG_USB_AUDIO_DEVICE

void OtgMode::OnUsbConnection(bool connected) { (void)connected; }
void OtgMode::Enter() {}
void OtgMode::Leave(bool by_keys, uint32_t key_mask) { (void)by_keys; (void)key_mask; }
void OtgMode::ExitByKeys(uint32_t key_mask) { (void)key_mask; }

#endif  // CONFIG_USB_AUDIO_DEVICE

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
                          "USB 音箱 + 麦克风已开启\nPC 音频由本机喇叭播放\n同时按下任意两个按键退出");

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