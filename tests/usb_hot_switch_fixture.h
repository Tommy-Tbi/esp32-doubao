// Host-only SDK fakes. Tests exercise the real mode manager and combo detector;
// USB enumeration, electrical behavior and audio DMA still require board tests.
#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#define CONFIG_USB_AUDIO_DEVICE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define PERIPH_RCC_ATOMIC() for (bool once = true; once; once = false)
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define eSetBits 0
using TaskHandle_t = void*;
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = -2;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
using nvs_handle_t = int;
inline std::string owner;
inline bool fail_install = false, fail_release = false, fail_save = false;
inline int boot_native = 0, notifications = 0, phy_installs = 0;
inline bool pad_enabled = true;
inline int64_t now_us = 0;
inline const char* esp_err_to_name(int) { return "fake-error"; }
inline int nvs_open(const char*, int, int* h) { *h = 1; return ESP_OK; }
inline int nvs_get_u8(int, const char*, uint8_t* v) { *v = boot_native; return ESP_OK; }
inline int nvs_set_u8(int, const char*, uint8_t v) {
    if (fail_save) return ESP_FAIL;
    boot_native = v;
    return ESP_OK;
}
inline int nvs_commit(int) { return ESP_OK; }
inline void nvs_close(int) {}
inline void usb_serial_jtag_ll_enable_bus_clock(bool) {}
inline void usb_serial_jtag_ll_phy_enable_pad(bool v) { pad_enabled = v; }
inline void usb_serial_jtag_ll_phy_disable_pull_override() {}
struct phy_context_t {};
using usb_phy_handle_t = phy_context_t*;
constexpr int USB_PHY_CTRL_SERIAL_JTAG = 1, USB_PHY_TARGET_INT = 0;
struct usb_phy_config_t { int controller; int target; };
inline int usb_new_phy(const usb_phy_config_t* cfg, usb_phy_handle_t* h) {
    assert(owner.empty());
    assert(cfg->controller == USB_PHY_CTRL_SERIAL_JTAG);
    if (fail_install) return ESP_FAIL;
    static phy_context_t phy;
    *h = &phy;
    owner = "native";
    ++phy_installs;
    return ESP_OK;
}
inline int usb_del_phy(usb_phy_handle_t h) {
    assert(h && owner == "native");
    if (fail_release) return ESP_FAIL;
    owner.clear();
    return ESP_OK;
}
inline int xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int,
                                  TaskHandle_t* task, int core) {
    assert(core == 0);
    *task = reinterpret_cast<void*>(1);
    return pdPASS;
}
inline void vTaskDelay(int) {}
inline void xTaskNotify(TaskHandle_t, uint32_t, int) { ++notifications; }
struct StopLoop {};
inline void xTaskNotifyWait(int, uint32_t, uint32_t*, uint32_t) { throw StopLoop{}; }
inline int64_t esp_timer_get_time() { return now_us += 1000; }

struct Display {
    std::string status, notice;
    void SetStatus(const char* s) { status = s; }
    void ShowNotification(const char* s, int) { notice = s; }
};
struct DisplayLockGuard { explicit DisplayLockGuard(Display*) {} };
struct lv_obj_t {};
constexpr int LV_OBJ_FLAG_SCROLLABLE = 1, LV_SIZE_CONTENT = 0,
    LV_OPA_COVER = 255, LV_TEXT_ALIGN_CENTER = 0, LV_OBJ_FLAG_HIDDEN = 2;
#define LV_PCT(x) (x)
inline lv_obj_t* lv_scr_act() { static lv_obj_t obj; return &obj; }
inline lv_obj_t* lv_obj_create(lv_obj_t*) { return lv_scr_act(); }
inline lv_obj_t* lv_label_create(lv_obj_t*) { return lv_scr_act(); }
inline int lv_color_hex(int v) { return v; }
#define LV_VOID(name) template<class... T> inline void name(T...) {}
LV_VOID(lv_obj_remove_flag) LV_VOID(lv_obj_set_size) LV_VOID(lv_obj_center)
LV_VOID(lv_obj_set_style_bg_opa) LV_VOID(lv_obj_set_style_bg_color)
LV_VOID(lv_obj_set_style_radius) LV_VOID(lv_obj_set_style_border_width)
LV_VOID(lv_obj_set_style_border_color) LV_VOID(lv_obj_set_style_pad_all)
LV_VOID(lv_obj_set_width) LV_VOID(lv_obj_set_style_text_align)
LV_VOID(lv_obj_set_style_text_color) LV_VOID(lv_label_set_text)
LV_VOID(lv_obj_clear_flag) LV_VOID(lv_obj_move_foreground) LV_VOID(lv_obj_add_flag)

struct UsbAudio {
    bool initialized = false, connected = false;
    bool fail_uninstall = false, fail_resume = false;
    static UsbAudio& GetInstance() { static UsbAudio usb; return usb; }
    bool IsInitialized() const { return initialized; }
    bool IsConnected() const { return connected; }
    void Start() {}
    void AddConnectionCallback(std::function<void(bool)>) {}
    bool Resume() {
        assert(owner.empty());
        if (fail_resume) return false;
        initialized = true;
        owner = "audio";
        return true;
    }
    bool Uninstall() {
        assert(owner == "audio");
        if (fail_uninstall) return false;
        initialized = connected = false;
        owner.clear();
        return true;
    }
};
