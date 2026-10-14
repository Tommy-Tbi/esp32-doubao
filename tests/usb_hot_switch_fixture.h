// Host-only SDK fakes. Tests exercise the real mode manager, PCM bridge and
// combo detector. USB enumeration, electrical behavior and DMA need board tests.
#pragma once
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#define CONFIG_USB_AUDIO_DEVICE 1
#define CONFIG_USB_AUDIO_SPEAKER 1
#define CONFIG_USB_AUDIO_MIC 1
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
inline int task_creates = 0, fail_task_at = -1;
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
    assert(core == 0 || core == 1);
    ++task_creates;
    if (task_creates == fail_task_at) return 0;
    if (task) *task = reinterpret_cast<void*>(1);
    return pdPASS;
}
inline void vTaskDelay(int) {}
inline void xTaskNotify(TaskHandle_t, uint32_t, int) { ++notifications; }
struct StopLoop {};
inline void xTaskNotifyWait(int, uint32_t, uint32_t*, uint32_t) { throw StopLoop{}; }
inline int64_t esp_timer_get_time() { return now_us; }

struct Display {
    std::string status, notice;
    int status_updates = 0;
    void SetStatus(const char* s) { status = s; notice.clear(); ++status_updates; }
    void ShowNotification(const char* s, int) { notice = s; }
};
struct AudioCodec {
    int input_rate = 48000, output_rate = 48000, channels = 1;
    int starts = 0, reads = 0, writes = 0;
    bool fail_read = false;
    std::function<void()> on_read;
    std::vector<int16_t> output;
    int input_sample_rate() const { return input_rate; }
    int output_sample_rate() const { return output_rate; }
    int input_channels() const { return channels; }
    int output_channels() const { return channels; }
    void Start() { ++starts; }
    bool InputData(std::vector<int16_t>& pcm) {
        ++reads;
        if (on_read) on_read();
        if (fail_read) return false;
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t(1000 + i);
        return true;
    }
    void OutputData(std::vector<int16_t>& pcm) { ++writes; output = pcm; }
};

struct UsbAudio {
    struct Config {
        int sample_rate, channels, bits_per_sample;
        bool speaker_enabled, mic_enabled;
    };
    bool initialized = false, connected = false;
    bool mic_active = false, fail_send = false;
    int mic_writes = 0;
    std::vector<int16_t> mic_pcm, speaker_pcm;
    std::function<void()> on_fetch;
    bool Initialize(const Config& c, bool enable) {
        assert(c.sample_rate == 48000 && c.channels == 1 && c.bits_per_sample == 16);
        assert(!enable);  // Bridge must not steal the mode worker's PHY ownership.
        return true;
    }
    bool IsMicrophoneActive() const { return mic_active && initialized && connected; }
    bool SendMicrophoneData(const int16_t* p, size_t n) {
        assert(IsMicrophoneActive());
        if (fail_send) return false;
        ++mic_writes; mic_pcm.assign(p, p + n); return true;
    }
    size_t AvailableSpeakerSamples() const {
        return initialized && connected ? speaker_pcm.size() : 0;
    }
    size_t FetchSpeakerData(int16_t* out, size_t n) {
        n = std::min(n, speaker_pcm.size());
        std::copy_n(speaker_pcm.begin(), n, out);
        speaker_pcm.erase(speaker_pcm.begin(), speaker_pcm.begin() + n);
        if (on_fetch) on_fetch();
        return n;
    }
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
