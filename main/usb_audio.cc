#include "usb_audio.h"
#include <esp_log.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include "usb_device_uac.h"

#define TAG "UsbAudio"

UsbAudio& UsbAudio::GetInstance() {
    static UsbAudio instance;
    return instance;
}

bool UsbAudio::Initialize() {
    if (initialized_) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing USB Audio Device");

    // Initialize USB Device UAC
    usb_device_uac_config_t config = {
        .sample_rate = 16000,
        .channel_num = 1,
        .bits_per_sample = 16,
    };

    esp_err_t ret = usb_device_uac_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Device UAC init failed: %s", esp_err_to_name(ret));
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "USB Audio Device initialized");
    return true;
}

void UsbAudio::Start() {
    if (!initialized_) {
        return;
    }
    running_ = true;
    ESP_LOGI(TAG, "USB Audio started");
}

void UsbAudio::Stop() {
    running_ = false;
    ESP_LOGI(TAG, "USB Audio stopped");
}

void UsbAudio::SendAudioData(const int16_t* pcm_data, size_t samples) {
    if (!running_ || !connected_) {
        return;
    }

    // Send audio data to USB
    usb_device_uac_send_data(pcm_data, samples * sizeof(int16_t));
}

void UsbAudio::CheckUsbConnection() {
    // Connection status is handled by USB Device UAC driver
}

void UsbAudio::SetConnected(bool connected) {
    if (connected_ != connected) {
        connected_ = connected;
        ESP_LOGI(TAG, "USB connection changed: %s", connected ? "connected" : "disconnected");
        if (connection_callback_) {
            connection_callback_(connected);
        }
    }
}

void UsbAudio::SetConnectionCallback(std::function<void(bool)> callback) {
    connection_callback_ = callback;
}