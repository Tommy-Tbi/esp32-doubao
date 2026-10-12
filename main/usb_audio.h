#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <memory>
#include <functional>
#include "esp_timer.h"

class UsbAudio {
public:
    static UsbAudio& GetInstance();

    // Initialize USB device as microphone
    bool Initialize();

    // Start USB audio transmission
    void Start();

    // Stop USB audio transmission
    void Stop();

    // Send PCM audio data to USB
    void SendAudioData(const int16_t* pcm_data, size_t samples);

    // Check if USB is connected
    bool IsConnected() const { return connected_; }

    // Set callback for connection status change
    void SetConnectionCallback(std::function<void(bool)> callback);

    // Called from USB driver callbacks
    void SetConnected(bool connected);

private:
    UsbAudio() = default;
    ~UsbAudio() = default;
    UsbAudio(const UsbAudio&) = delete;
    UsbAudio& operator=(const UsbAudio&) = delete;

    bool initialized_ = false;
    bool running_ = false;
    bool connected_ = false;
    std::function<void(bool)> connection_callback_;
    esp_timer_handle_t usb_check_timer_ = nullptr;

    void CheckUsbConnection();
};