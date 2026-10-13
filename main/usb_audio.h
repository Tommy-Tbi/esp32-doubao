#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <functional>
#include <atomic>

// USB Audio Device：USB 复合音频设备（USB Speaker + USB Microphone）
//
//   PC --(USB Speaker)--> ESP32   : PC 播放的音频由设备本地喇叭播出（FetchSpeakerData）
//   ESP32 --(USB Mic)--> PC       : 设备麦克风音频上传给 PC（SendMicrophoneData）
//
// 固定格式：16bit PCM，48kHz，单声道（与设备 codec 匹配）。
// 基于 TinyUSB 的 UAC2 设备类实现。
class UsbAudio {
public:
    static UsbAudio& GetInstance();

    struct Config {
        int sample_rate = 48000;
        int channels = 1;
        int bits_per_sample = 16;
        bool speaker_enabled = true;  // USB Speaker (host -> device)
        bool mic_enabled = true;      // USB Microphone (device -> host)
    };

    // 初始化 USB 设备栈（内部会启动 TinyUSB 任务）
    bool Initialize(const Config& config);

    void Start();
    void Stop();

    bool IsInitialized() const { return initialized_; }
    bool IsConnected() const { return connected_; }  // 已挂载到主机
    bool IsSpeakerEnabled() const { return config_.speaker_enabled; }
    bool IsMicEnabled() const { return config_.mic_enabled; }

    int sample_rate() const { return config_.sample_rate; }
    int channels() const { return config_.channels; }

    // ---- USB Microphone：设备 -> PC ----
    // 麦克风 PCM 入队，内部任务按 USB 节奏送主机
    bool SendMicrophoneData(const int16_t* pcm, size_t samples);

    // ---- USB Speaker：PC -> 设备 ----
    // PC 送来的音频可直接取出播放（内部 TinyUSB FIFO）
    size_t AvailableSpeakerSamples() const;
    size_t FetchSpeakerData(int16_t* out, size_t max_samples);

    void SetConnectionCallback(std::function<void(bool)> callback);

    // ---- 供 tinyusb 回调调用 ----
    void OnMounted();
    void OnUnmounted();
    void FeedMicrophone();
    void OnMicrophoneInterface(bool active);

private:
    UsbAudio() = default;
    ~UsbAudio() = default;
    UsbAudio(const UsbAudio&) = delete;
    UsbAudio& operator=(const UsbAudio&) = delete;

    bool initialized_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> mic_streaming_{false};
    Config config_;

    void* mic_ringbuf_ = nullptr;  // FreeRTOS RingbufHandle_t，设备麦克风待发数据

    std::function<void(bool)> connection_callback_;
};
