#include "usb_audio.h"

#include <string.h>

#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>

#include "tinyusb.h"
#include "tusb.h"

#define TAG "UsbAudio"

// ---------------------------------------------------------------------------
// USB 复合音频设备描述符（UAC2）
//
//   func 0: Speaker  (IAD itf 0/1) - PC 播放 -> 设备喇叭，带反馈端点
//   func 1: Microphone(IAD itf 2/3) - 设备麦克风 -> PC
//
// 全速模式，1ms 帧，端点最大包长 64 字节。
// ---------------------------------------------------------------------------
#define UAC_SAMPLE_RATE      48000
#define UAC_SAMPLE_BITS      16
#define UAC_EP_SIZE          64
#define EPNUM_SPK_OUT        0x01
#define EPNUM_SPK_FB_IN      0x81
#define EPNUM_MIC_IN         0x82

#define ITF_SPK_AC           0
#define ITF_SPK_AS           1
#define ITF_MIC_AC           2
#define ITF_MIC_AS           3
#define ITF_NUM_TOTAL        4

// tinyusb 音频函数号
#define AUDIO_FUNC_SPEAKER   0
#define AUDIO_FUNC_MIC       1

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO20_SPEAKER_MONO_FB_DESC_LEN + \
                          TUD_AUDIO20_MIC_ONE_CH_DESC_LEN)

static tusb_desc_device_t const kDescDevice = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // 音频类需要用 IAD 包裹（subclass = common, protocol = IAD）
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4001 + (1 << 4),  // AUDIO bit，PC 会为该组合保存驱动
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static uint8_t const kDescConfig[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 250),

    // --- USB Speaker (48kHz/16bit/mono, 带反馈端点) ---
    TUD_AUDIO20_SPEAKER_MONO_FB_DESCRIPTOR(ITF_SPK_AC, 0x04,
                                            UAC_SAMPLE_BITS / 8, UAC_SAMPLE_BITS,
                                            EPNUM_SPK_OUT, UAC_EP_SIZE,
                                            EPNUM_SPK_FB_IN, 0x03),

    // --- USB Microphone (48kHz/16bit/mono) ---
    TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR(ITF_MIC_AC, 0x04,
                                      UAC_SAMPLE_BITS / 8, UAC_SAMPLE_BITS,
                                      EPNUM_MIC_IN, UAC_EP_SIZE),
};

static char const* kStringDescArr[] = {
    (const char[]){0x09, 0x04},   // 0: English (0x0409)
    "ESP32-S3",                  // 1: Manufacturer
    "ESP32-S3 USB Speaker+Mic",   // 2: Product
    "0001",                      // 3: Serial
    "UAC2 Speaker+Mic",          // 4: Audio Interface
};

extern "C" {

// 设备/配置/字符串描述符通过 tinyusb_config_t 传给 esp_tinyusb，
// 这里不需要（也不应该）再实现 tud_descriptor_*_cb，否则会重复定义。

void tud_mount_cb(void) {
    UsbAudio::GetInstance().OnMounted();
}

void tud_umount_cb(void) {
    UsbAudio::GetInstance().OnUnmounted();
}

// 音量/静音控制由本地 codec 处理，这里直接应答成功
void tud_audio_set_volume(uint8_t rhport, uint8_t itfnum, int16_t min, int16_t max, int16_t cur) {
    (void)rhport; (void)itfnum; (void)min; (void)max; (void)cur;
}

int16_t tud_audio_get_volume(uint8_t rhport, uint8_t itfnum) {
    (void)rhport; (void)itfnum;
    return 0;  // 0dB
}

void tud_audio_set_mute(uint8_t rhport, uint8_t itfnum, bool mute) {
    (void)rhport; (void)itfnum; (void)mute;
}

bool tud_audio_get_mute(uint8_t rhport, uint8_t itfnum) {
    (void)rhport; (void)itfnum;
    return false;
}

}  // extern "C"

// ---------------------------------------------------------------------------
UsbAudio& UsbAudio::GetInstance() {
    static UsbAudio instance;
    return instance;
}

bool UsbAudio::Initialize(const Config& config) {
    if (initialized_) {
        return true;
    }
    config_ = config;
    ESP_LOGI(TAG, "USB audio: rate=%d ch=%d bits=%d speaker=%d mic=%d",
             config_.sample_rate, config_.channels, config_.bits_per_sample,
             config_.speaker_enabled, config_.mic_enabled);

    if (config_.mic_enabled) {
        // 200ms 缓冲
        size_t bytes = (size_t)config_.sample_rate * config_.channels * sizeof(int16_t) / 5;
        mic_ringbuf_ = xRingbufferCreate(bytes, RINGBUF_TYPE_BYTEBUF);
        if (mic_ringbuf_ == nullptr) {
            ESP_LOGE(TAG, "mic ringbuf alloc failed (%u)", (unsigned)bytes);
            return false;
        }
    }

    const tinyusb_config_t tusb_config = {
        .device_descriptor = &kDescDevice,
        .string_descriptor = kStringDescArr,
        .string_descriptor_count = sizeof(kStringDescArr) / sizeof(kStringDescArr[0]),
        .external_phy = false,
        .configuration_descriptor = kDescConfig,
        .self_powered = false,
        .vbus_monitor_io = -1,
    };
    if (tinyusb_driver_install(&tusb_config) != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed");
        return false;
    }

    if (config_.mic_enabled) {
        xTaskCreatePinnedToCore(MicFeedTaskEntry, "usb_mic_tx", 3072, this, 6, (TaskHandle_t*)&mic_task_handle_, 0);
    }

    initialized_ = true;
    running_ = true;
    ESP_LOGI(TAG, "USB audio device initialized (speaker+mic)");
    return true;
}

void UsbAudio::Start() {
    running_ = true;
}

void UsbAudio::Stop() {
    running_ = false;
    OnUnmounted();
}

bool UsbAudio::SendMicrophoneData(const int16_t* pcm, size_t samples) {
    if (!running_ || mic_ringbuf_ == nullptr || samples == 0) {
        return false;
    }
    RingbufHandle_t rb = (RingbufHandle_t)mic_ringbuf_;
    size_t bytes = samples * sizeof(int16_t);
    if (xRingbufferGetCurFreeSize(rb) < bytes) {
        // 缓冲满：丢最旧数据，保证实时性
        size_t item_size = 0;
        uint8_t* item = (uint8_t*)xRingbufferReceiveUpTo(rb, &item_size, 0, bytes);
        if (item != nullptr) {
            vRingbufferReturnItem(rb, item);
        }
    }
    return xRingbufferSend(rb, pcm, bytes, 0) == pdTRUE;
}

size_t UsbAudio::AvailableSpeakerSamples() const {
    if (!initialized_ || !config_.speaker_enabled || !connected_) {
        return 0;
    }
    return tud_audio_n_available(AUDIO_FUNC_SPEAKER) / sizeof(int16_t);
}

size_t UsbAudio::FetchSpeakerData(int16_t* out, size_t max_samples) {
    if (max_samples == 0) {
        return 0;
    }
    if (max_samples > UINT16_MAX / sizeof(int16_t)) {
        max_samples = UINT16_MAX / sizeof(int16_t);
    }
    uint16_t want = (uint16_t)(max_samples * sizeof(int16_t));
    uint16_t got = tud_audio_n_read(AUDIO_FUNC_SPEAKER, out, want);
    return got / sizeof(int16_t);
}

void UsbAudio::MicFeedTaskEntry(void* arg) {
    static_cast<UsbAudio*>(arg)->MicFeedTask();
}

void UsbAudio::MicFeedTask() {
    RingbufHandle_t rb = (RingbufHandle_t)mic_ringbuf_;
    int16_t chunk[64];
    uint32_t fb_counter = 0;

    while (running_) {
        if (!connected_) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t item_size = 0;
        uint8_t* item = (uint8_t*)xRingbufferReceiveUpTo(rb, &item_size, 0, sizeof(chunk));
        if (item != nullptr) {
            tud_audio_n_write(AUDIO_FUNC_MIC, item, (uint16_t)item_size);
            vRingbufferReturnItem(rb, item);
            tud_audio_n_clear_ep_in_ff(AUDIO_FUNC_MIC);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        // Speaker 反馈通道：16ms 一次
        if (++fb_counter >= 16) {
            fb_counter = 0;
            UpdateSpeakerFeedback();
        }
    }

    mic_task_handle_ = nullptr;
    vTaskDelete(NULL);
}

void UsbAudio::UpdateSpeakerFeedback() {
    if (!connected_ || !config_.speaker_enabled) {
        return;
    }
    // 反馈值为 16.16 定点（全速模式下驱动内部再转成 10.14）：
    // Ff = 每个 1ms 帧的样本数 = sample_rate / 1000
    uint32_t fb = (uint32_t)(config_.sample_rate / 1000) << 16;
    tud_audio_n_fb_set(AUDIO_FUNC_SPEAKER, fb);
}

void UsbAudio::OnMounted() {
    if (!connected_) {
        connected_ = true;
        ESP_LOGI(TAG, "USB host connected");
        UpdateSpeakerFeedback();
        if (connection_callback_) {
            connection_callback_(true);
        }
    }
}

void UsbAudio::OnUnmounted() {
    if (connected_) {
        connected_ = false;
        ESP_LOGI(TAG, "USB host disconnected");
        if (connection_callback_) {
            connection_callback_(false);
        }
    }
}

void UsbAudio::SetConnectionCallback(std::function<void(bool)> callback) {
    connection_callback_ = callback;
}
