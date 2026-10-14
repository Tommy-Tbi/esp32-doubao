#ifndef SPARKBOT_OTG_MODE_H_
#define SPARKBOT_OTG_MODE_H_

#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "display.h"

// ESP32-S3 internal-PHY ownership: UAC2 OTG <-> native USB Serial/JTAG.
// Switching is serialized on a dedicated task, never in button/USB callbacks.
class OtgMode {
public:
    static OtgMode& GetInstance();
    // Call after the audio bridge has prepared its USB configuration.
    void Begin(Display* display);
    bool IsUsbConnected() const { return usb_connected_.load(); }
    bool IsActive() const { return active_.load(); }
    bool IsBlocked() const { return native_.load(); }
    void ToggleByKeys(uint32_t key_mask = 0);
    static const char* StatusText() { return "USB 音频"; }

private:
    OtgMode() = default;
    void Run();
    const char* SwitchMode();
    void OnUsbConnection(bool connected);
    void Enter();
    enum class PanelState { Hidden, Audio, ToNative, ToAudio, Error };
    void ShowOverlay(PanelState state);
    void SetUsbStatus(const char* text);
    void HideOverlay();

    Display* display_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> usb_connected_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> native_{false};
    std::atomic<bool> switching_{false};
    bool faulted_ = false;  // Worker only; incomplete teardown requires reset.
    const char* status_text_ = nullptr;  // Worker only; avoid clearing live notices
    PanelState panel_state_ = PanelState::Hidden;  // Protected by DisplayLockGuard
    void* overlay_ = nullptr;
    void* badge_ = nullptr;
    void* title_ = nullptr;
    void* detail_ = nullptr;
    void* hint_ = nullptr;
};

#endif
