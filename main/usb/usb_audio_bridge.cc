#include "usb_audio_bridge.h"
#include "usb_audio.h"

#include <algorithm>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "UsbBridge"

bool UsbAudioBridge::Initialize(AudioCodec* codec) {
    if (!codec || codec->input_sample_rate() != 48000 ||
        codec->output_sample_rate() != 48000 || codec->input_channels() != 1 ||
        codec->output_channels() != 1) {
        ESP_LOGE(TAG, "USB bridge requires native 48000 Hz, mono input/output");
        return false;
    }
    codec_ = codec;
    codec_->Start();
    const UsbAudio::Config config = {
        .sample_rate = 48000,
        .channels = 1,
        .bits_per_sample = 16,
#if CONFIG_USB_AUDIO_SPEAKER
        .speaker_enabled = true,
#else
        .speaker_enabled = false,
#endif
#if CONFIG_USB_AUDIO_MIC
        .mic_enabled = true,
#else
        .mic_enabled = false,
#endif
    };
    // The mode worker owns installation and all subsequent PHY handoffs.
    return UsbAudio::GetInstance().Initialize(config, false);
}

bool UsbAudioBridge::Start() {
    if (started_) return true;
    if (!codec_ || failed_) return false;
#if CONFIG_FREERTOS_UNICORE
    constexpr int kAudioCore = 0;
#else
    constexpr int kAudioCore = 1;
#endif
    // Keep both codec directions open throughout mode changes. ES8311 is
    // duplex, and the shared PA/backlight pin must never be power-cycled here.
    if (xTaskCreatePinnedToCore([](void* p) {
        static_cast<UsbAudioBridge*>(p)->CaptureTask();
    }, "usb_capture", 4096, this, 5, nullptr, kAudioCore) != pdPASS) {
        failed_ = true;
        return false;
    }
    // A failed second task is fatal at the entry point; never start a second
    // capture reader by retrying this partially started service.
    started_ = xTaskCreatePinnedToCore([](void* p) {
        static_cast<UsbAudioBridge*>(p)->PlaybackTask();
    }, "usb_playback", 4096, this, 5, nullptr, kAudioCore) == pdPASS;
    failed_ = !started_;
    return started_;
}

bool UsbAudioBridge::CaptureBlock(std::vector<int16_t>& pcm) {
    auto& usb = UsbAudio::GetInstance();
    const bool recording = usb.IsMicrophoneActive();
    // Always drain I2S, even in native JTAG mode or while the host isn't
    // recording. This prevents old DMA samples leaking into a new recording.
    auto clear_meter = [this]() {
        microphone_meter_.Reset();
        microphone_level_ = 0;
    };
    if (!codec_->InputData(pcm)) {
        std::fill(pcm.begin(), pcm.end(), 0);
        clear_meter();
        return false;
    }
    if (!recording || !usb.IsMicrophoneActive()) {
        clear_meter();
        return true;
    }
    // Meter the same successful capture blocks that are queued for USB, not
    // speaker PCM. Muting still drains capture and sends silence in FeedMicrophone.
    const bool sent = usb.SendMicrophoneData(pcm.data(), pcm.size());
    if (!sent || usb.IsMicrophoneMuted() || !usb.IsMicrophoneActive()) clear_meter();
    else microphone_level_ = microphone_meter_.Update(pcm.data(), pcm.size());
    return sent;
}

void UsbAudioBridge::PlaybackBlock(std::vector<int16_t>& pcm) {
    auto& usb = UsbAudio::GetInstance();
    std::fill(pcm.begin(), pcm.end(), 0);
    if (usb.AvailableSpeakerSamples() >= pcm.size()) {
        const size_t got = usb.FetchSpeakerData(pcm.data(), pcm.size());
        if (!usb.IsConnected()) std::fill(pcm.begin(), pcm.end(), 0);
        else std::fill(pcm.begin() + got, pcm.end(), 0);
    }
    // Blocking I2S writes clock the task; silence keeps the duplex codec and
    // backlight stable while disconnected, suspended or in Serial/JTAG mode.
    playback_level_ = level_meter_.Update(pcm.data(), pcm.size());
    codec_->OutputData(pcm);
}

void UsbAudioBridge::CaptureTask() {
    std::vector<int16_t> pcm(kBlockSamples);
    uint32_t blocks = 0, failures = 0;
    int peak = 0;
    int64_t last_log = esp_timer_get_time();
    for (;;) {
        if (CaptureBlock(pcm)) ++blocks;
        else {
            ++failures;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        for (int16_t value : pcm) {
            const int magnitude = value < 0 ? -static_cast<int>(value) : value;
            peak = std::max(peak, magnitude);
        }
        const int64_t now = esp_timer_get_time();
        if (now - last_log >= 5000000) {
            ESP_LOGI(TAG, "mic: host_recording=%d blocks=%lu failures=%lu peak=%d",
                     UsbAudio::GetInstance().IsMicrophoneActive(),
                     static_cast<unsigned long>(blocks),
                     static_cast<unsigned long>(failures), peak);
            blocks = failures = 0;
            peak = 0;
            last_log = now;
        }
    }
}

void UsbAudioBridge::PlaybackTask() {
    std::vector<int16_t> pcm(kBlockSamples);
    for (;;) PlaybackBlock(pcm);
}
