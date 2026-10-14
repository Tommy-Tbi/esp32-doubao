#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>

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

    // 配置只在首次调用时保存。enable=false 仅准备配置，不占用 PHY；
    // enable=true 安装设备栈并启动可协作退出的 USB 任务。
    bool Initialize(const Config& config, bool enable = true);
    // Reinstall using the immutable configuration prepared at boot.
    bool Resume();

    void Start();
    void Stop();
    bool Uninstall();

    bool IsInitialized() const { return initialized_; }
    bool IsConnected() const { return connected_; }  // 已挂载到主机
    bool IsSpeakerEnabled() const { return config_.speaker_enabled; }
    bool IsMicEnabled() const { return config_.mic_enabled; }
    bool IsMicrophoneActive() const {
        return config_.mic_enabled && initialized_.load() && running_.load() &&
               connected_.load() && mic_streaming_.load();
    }

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
    // 追加一个连接状态监听者（SetConnectionCallback 会清掉已有的监听者）
    void AddConnectionCallback(std::function<void(bool)> callback);

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

    void NotifyConnection(bool connected);
    bool StartUsbTask();
    void StopUsbTask();

    std::atomic<bool> initialized_{false};
    bool configured_ = false;
    // Application I/O is excluded while the USB task/PHY is being destroyed.
    // TinyUSB callbacks never take this mutex (uninstall waits for that task).
    mutable std::mutex io_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> mic_streaming_{false};
    Config config_;

    bool task_running_ = false;  // Accessed only by the lifecycle owner
    std::atomic<bool> task_stop_{false};
    void* task_done_ = nullptr;  // FreeRTOS binary semaphore
    void* mic_ringbuf_ = nullptr;  // FreeRTOS RingbufHandle_t，设备麦克风待发数据

    std::mutex callback_mutex_;
    std::vector<std::function<void(bool)>> connection_callbacks_;
};
