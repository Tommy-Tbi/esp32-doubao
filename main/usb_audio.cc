#include "usb_audio.h"

#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "tinyusb.h"
#include "tusb.h"

#define TAG "UsbAudio"

// esp_tinyusb 1.x silently skips stack teardown with older TinyUSB builds.
// Such builds cannot safely reinstall the DWC2 driver after a PHY handoff.
#ifndef tusb_teardown
#error "USB hot switching requires Espressif TinyUSB with tusb_teardown support"
#endif
#if !CONFIG_TINYUSB_NO_DEFAULT_TASK || CONFIG_TINYUSB_INIT_IN_DEFAULT_TASK
#error "Hot switching requires CONFIG_TINYUSB_NO_DEFAULT_TASK=y and CONFIG_TINYUSB_INIT_IN_DEFAULT_TASK=n"
#endif

// Full-speed UAC2: two independent functions, each with AC + AS interfaces.
// 48 samples/ms * 2 bytes, plus one sample of asynchronous clock tolerance.
#define UAC_SAMPLE_RATE 48000
#define UAC_EP_SIZE 98
#define AUDIO_FUNC_SPEAKER 0
#define AUDIO_FUNC_MIC 1
#define ITF_SPK_AC 0
#define ITF_SPK_AS 1
#define ITF_MIC_AC 2
#define ITF_MIC_AS 3
#define EPNUM_SPK_OUT 0x01
#define EPNUM_SPK_FB_IN 0x81
#define EPNUM_MIC_IN 0x82
#define ITF_MEDIA 4
#define EPNUM_MEDIA_IN 0x83

static const uint8_t kMediaReportDesc[] = { TUD_HID_REPORT_DESC_CONSUMER() };
static const uint8_t kMediaDesc[] = {
    TUD_HID_DESCRIPTOR(ITF_MEDIA, 5, HID_ITF_PROTOCOL_NONE,
        sizeof(kMediaReportDesc), EPNUM_MEDIA_IN, 2, 10),
};
static_assert(CFG_TUD_HID == 1);

// Deliberately no Feature Unit: volume is controlled locally. Do not advertise
// mute/volume/connector controls unless their class requests are implemented.
#define AUDIO_COMMON_DESC(itf, category, input_type, output_type, stream_term, eps, stridx) \
    TUD_AUDIO_DESC_IAD(itf, 2, stridx), \
    TUD_AUDIO_DESC_STD_AC(itf, 0, stridx), \
    TUD_AUDIO_DESC_CS_AC(0x0200, category, TUD_AUDIO_DESC_CLK_SRC_LEN + \
        TUD_AUDIO_DESC_INPUT_TERM_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN, 0), \
    TUD_AUDIO_DESC_CLK_SRC(4, AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK, \
        AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS, 0, 0), \
    TUD_AUDIO_DESC_INPUT_TERM(1, input_type, 0, 4, 1, 0, 0, 0, stridx), \
    TUD_AUDIO_DESC_OUTPUT_TERM(3, output_type, 0, 1, 4, 0, stridx), \
    TUD_AUDIO_DESC_STD_AS_INT(itf + 1, 0, 0, stridx), \
    TUD_AUDIO_DESC_STD_AS_INT(itf + 1, 1, eps, stridx), \
    TUD_AUDIO_DESC_CS_AS_INT(stream_term, 0, AUDIO_FORMAT_TYPE_I, \
        AUDIO_DATA_FORMAT_TYPE_I_PCM, 1, 0, 0), \
    TUD_AUDIO_DESC_TYPE_I_FORMAT(2, 16)

static const uint8_t kSpeakerDesc[] = {
    AUDIO_COMMON_DESC(ITF_SPK_AC, AUDIO_FUNC_DESKTOP_SPEAKER,
        AUDIO_TERM_TYPE_USB_STREAMING, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, 1, 2, 4),
    TUD_AUDIO_DESC_STD_AS_ISO_EP(EPNUM_SPK_OUT,
        TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS, UAC_EP_SIZE, 1),
    TUD_AUDIO_DESC_CS_AS_ISO_EP(0, 0, 0, 0),
    // UAC2 feedback uses 16.16 (4 bytes), including at full speed.
    TUD_AUDIO_DESC_STD_AS_ISO_FB_EP(EPNUM_SPK_FB_IN, 4, 1),
};
static const uint8_t kMicDesc[] = {
    AUDIO_COMMON_DESC(ITF_MIC_AC, AUDIO_FUNC_MICROPHONE,
        AUDIO_TERM_TYPE_IN_GENERIC_MIC, AUDIO_TERM_TYPE_USB_STREAMING, 3, 1, 6),
    TUD_AUDIO_DESC_STD_AS_ISO_EP(EPNUM_MIC_IN,
        TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS, UAC_EP_SIZE, 1),
    TUD_AUDIO_DESC_CS_AS_ISO_EP(0, 0, 0, 0),
};
static_assert(sizeof(kSpeakerDesc) == CFG_TUD_AUDIO_FUNC_1_DESC_LEN);
static_assert(sizeof(kMicDesc) == CFG_TUD_AUDIO_FUNC_2_DESC_LEN);
static_assert(CFG_TUD_AUDIO == 2);
static constexpr size_t kConfigLen = TUD_CONFIG_DESC_LEN + sizeof(kSpeakerDesc) +
    sizeof(kMicDesc) + sizeof(kMediaDesc);
static uint8_t kDescConfig[kConfigLen] = {
    TUD_CONFIG_DESCRIPTOR(1, 5, 0, kConfigLen, 0, 250),
};
static const tusb_desc_device_t kDescDevice = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4011,
    .bcdDevice = 0x0103,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};
static const char kLanguage[] = {0x09, 0x04};
static const char* kStrings[] = {
    kLanguage, "ESP32-S3", "ESP32-S3 USB Speaker+Mic", "0001", "ESP32S3 Speaker",
    "Media Control", "ESP32S3 MIC",
};

extern "C" {
const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance) {
    return instance == 0 ? kMediaReportDesc : nullptr;
}
uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t,
                             uint8_t*, uint16_t) {
    return 0;  // No polled status or feature reports.
}
void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t,
                           const uint8_t*, uint16_t) {}

void tud_mount_cb(void) { UsbAudio::GetInstance().OnMounted(); }
void tud_umount_cb(void) { UsbAudio::GetInstance().OnUnmounted(); }

bool tud_audio_get_req_entity_cb(uint8_t rhport, const tusb_control_request_t* request) {
    const uint8_t itf = request->wIndex & 0xff;
    const uint8_t entity = request->wIndex >> 8;
    const uint8_t selector = request->wValue >> 8;
    if ((itf != ITF_SPK_AC && itf != ITF_MIC_AC) || entity != 4 ||
        (request->wValue & 0xff) != 0) return false;

    if (selector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            uint32_t rate = UAC_SAMPLE_RATE;
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &rate, sizeof(rate));
        }
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            // One subrange: MIN=MAX=48000, RES=0 (little endian).
            uint8_t range[] = {1, 0, 0x80, 0xbb, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, range, sizeof(range));
        }
    }
    if (selector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
        uint8_t valid = 1;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &valid, sizeof(valid));
    }
    return false;
}

void tud_audio_feedback_params_cb(uint8_t, uint8_t, audio_feedback_params_t* params) {
    // TinyUSB adjusts feedback from the speaker RX FIFO, independently of mic.
    params->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    params->sample_freq = UAC_SAMPLE_RATE;
}

bool tud_audio_tx_done_pre_load_cb(uint8_t, uint8_t func, uint8_t, uint8_t alt) {
    if (func == AUDIO_FUNC_MIC && alt != 0) UsbAudio::GetInstance().FeedMicrophone();
    return true;
}

bool tud_audio_set_itf_cb(uint8_t, const tusb_control_request_t* request) {
    if ((request->wIndex & 0xff) == ITF_MIC_AS) {
        UsbAudio::GetInstance().OnMicrophoneInterface(request->wValue == 1);
    }
    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t, const tusb_control_request_t* request) {
    // TinyUSB 0.17 calls the close callback (not the final open callback) when
    // the host switches to alternate 0. Stop enqueueing immediately on close.
    if ((request->wIndex & 0xff) == ITF_MIC_AS) {
        UsbAudio::GetInstance().OnMicrophoneInterface(false);
    }
    return true;
}
}  // extern "C"

UsbAudio& UsbAudio::GetInstance() {
    static UsbAudio instance;
    return instance;
}

bool UsbAudio::Initialize(const Config& config, bool enable) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (initialized_) return true;
    if (config.sample_rate != UAC_SAMPLE_RATE || config.channels != 1 || config.bits_per_sample != 16) {
        ESP_LOGE(TAG, "USB descriptors require 48000 Hz / mono / 16 bit");
        return false;
    }
    if (!configured_) {
        config_ = config;
        configured_ = true;
    }
    if (!enable) return true;
    if (config_.mic_enabled) {
        mic_ringbuf_ = xRingbufferCreate(UAC_SAMPLE_RATE * sizeof(int16_t) / 5, RINGBUF_TYPE_BYTEBUF);
        if (!mic_ringbuf_) return false;
    }
    memcpy(kDescConfig + TUD_CONFIG_DESC_LEN, kSpeakerDesc, sizeof(kSpeakerDesc));
    memcpy(kDescConfig + TUD_CONFIG_DESC_LEN + sizeof(kSpeakerDesc), kMicDesc, sizeof(kMicDesc));
    memcpy(kDescConfig + TUD_CONFIG_DESC_LEN + sizeof(kSpeakerDesc) + sizeof(kMicDesc),
        kMediaDesc, sizeof(kMediaDesc));
    media_.Reset();
    const tinyusb_config_t tusb_config = {
        .device_descriptor = &kDescDevice,
        .string_descriptor = kStrings,
        .string_descriptor_count = sizeof(kStrings) / sizeof(kStrings[0]),
        .external_phy = false,
        .configuration_descriptor = kDescConfig,
        .self_powered = false,
        .vbus_monitor_io = -1,
    };
    running_ = true;
    initialized_ = true;  // Mount callbacks can run before install returns.
    const esp_err_t err = tinyusb_driver_install(&tusb_config);
    if (err != ESP_OK) {
        initialized_ = false;
        running_ = false;
        if (mic_ringbuf_) vRingbufferDelete((RingbufHandle_t)mic_ringbuf_);
        mic_ringbuf_ = nullptr;
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return false;
    }
    if (!StartUsbTask()) {
        running_ = false;
        connected_ = false;
        if (tinyusb_driver_uninstall() == ESP_OK) {
            initialized_ = false;
            if (mic_ringbuf_) vRingbufferDelete((RingbufHandle_t)mic_ringbuf_);
            mic_ringbuf_ = nullptr;
        }
        return false;
    }
    ESP_LOGI(TAG, "USB audio initialized; native USB Serial/JTAG shares this PHY");
    return true;
}

bool UsbAudio::StartUsbTask() {
    if (!task_done_) task_done_ = xSemaphoreCreateBinary();
    if (!task_done_) return false;
    task_stop_ = false;
    // Unlike esp_tinyusb 1.x's default task, this task is joined cooperatively.
    // Never delete a task while it owns TinyUSB FIFO/callback mutexes.
    task_running_ = xTaskCreatePinnedToCore([](void* arg) {
        auto* self = static_cast<UsbAudio*>(arg);
        while (!self->task_stop_.load()) {
            tud_task_ext(10, false);
            self->media_.Poll(self->running_ && self->connected_,
                tud_hid_ready(), [](uint16_t usage) {
                    const uint8_t report[] = {
                        static_cast<uint8_t>(usage), static_cast<uint8_t>(usage >> 8)
                    };
                    return tud_hid_report(0, report, sizeof(report));
                });
        }
        xSemaphoreGive((SemaphoreHandle_t)self->task_done_);
        vTaskDelete(nullptr);
    }, "usb_audio", 4096, this, 5, nullptr, 0) == pdPASS;
    return task_running_;
}

void UsbAudio::StopUsbTask() {
    if (!task_running_) return;
    task_stop_ = true;
    xSemaphoreTake((SemaphoreHandle_t)task_done_, portMAX_DELAY);
    task_running_ = false;
}

bool UsbAudio::Resume() {
    // Configuration is set once by the audio bridge before the mode worker starts.
    return configured_ && Initialize(config_);
}

bool UsbAudio::RequestPlayPause() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!initialized_ || !running_ || !connected_ || !config_.speaker_enabled) return false;
    return media_.Request();
}

void UsbAudio::Start() { if (initialized_) running_ = true; }
void UsbAudio::Stop() { running_ = false; }

bool UsbAudio::Uninstall() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!initialized_) return true;
    running_ = false;
    connected_ = false;
    mic_streaming_ = false;
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));  // Host must observe a real detach.
    StopUsbTask();
    media_.Reset();
    connected_ = false;
    mic_streaming_ = false;
    const esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_uninstall: %s", esp_err_to_name(err));
        // Keep buffers alive: the USB task may still reference them.
        return false;
    }
    if (mic_ringbuf_) {
        vRingbufferDelete((RingbufHandle_t)mic_ringbuf_);
        mic_ringbuf_ = nullptr;
    }
    initialized_ = false;
    connected_ = false;
    mic_streaming_ = false;
    ESP_LOGI(TAG, "USB audio uninstalled");
    return true;
}

bool UsbAudio::SendMicrophoneData(const int16_t* pcm, size_t samples) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!running_ || !connected_ || !mic_streaming_ || !mic_ringbuf_ || !pcm || !samples) return false;
    return xRingbufferSend((RingbufHandle_t)mic_ringbuf_, pcm, samples * sizeof(int16_t), 0) == pdTRUE;
}

void UsbAudio::OnMicrophoneInterface(bool active) {
    if (!initialized_) return;
    mic_streaming_ = false;
    if (mic_ringbuf_) {
        size_t bytes;
        void* item;
        while ((item = xRingbufferReceive((RingbufHandle_t)mic_ringbuf_, &bytes, 0)) != nullptr) {
            vRingbufferReturnItem((RingbufHandle_t)mic_ringbuf_, item);
        }
    }
    mic_streaming_ = active;
    ESP_LOGI(TAG, "USB microphone stream %s", active ? "started" : "stopped");
}

void UsbAudio::FeedMicrophone() {
    // Called by the USB task once per IN transfer, not by an unrelated RTOS timer.
    // Pad underruns with silence; never clear the FIFO immediately after writing.
    uint8_t frame[96] = {};
    size_t copied = 0;
    if (running_ && mic_ringbuf_) {
        while (copied < sizeof(frame)) {
            size_t bytes = 0;
            void* item = xRingbufferReceiveUpTo((RingbufHandle_t)mic_ringbuf_, &bytes, 0, sizeof(frame) - copied);
            if (!item) break;
            memcpy(frame + copied, item, bytes);
            copied += bytes;
            vRingbufferReturnItem((RingbufHandle_t)mic_ringbuf_, item);
        }
    }
    tud_audio_n_write(AUDIO_FUNC_MIC, frame, sizeof(frame));
}

size_t UsbAudio::AvailableSpeakerSamples() const {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!initialized_ || !config_.speaker_enabled || !connected_ || !running_) return 0;
    return tud_audio_n_available(AUDIO_FUNC_SPEAKER) / sizeof(int16_t);
}

size_t UsbAudio::FetchSpeakerData(int16_t* out, size_t max_samples) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!out || !max_samples || !initialized_ || !config_.speaker_enabled ||
        !connected_ || !running_) return 0;
    if (max_samples > UINT16_MAX / sizeof(int16_t)) max_samples = UINT16_MAX / sizeof(int16_t);
    return tud_audio_n_read(AUDIO_FUNC_SPEAKER, out, max_samples * sizeof(int16_t)) / sizeof(int16_t);
}

void UsbAudio::OnMounted() {
    if (!initialized_) return;  // Native Serial/JTAG has no TinyUSB audio stack.
    connected_ = true;
    ESP_LOGI(TAG, "USB host connected");
    NotifyConnection(true);
}
void UsbAudio::OnUnmounted() {
    if (!initialized_) return;
    media_.Reset();
    connected_ = false;
    mic_streaming_ = false;
    ESP_LOGI(TAG, "USB host disconnected");
    NotifyConnection(false);
}
void UsbAudio::NotifyConnection(bool connected) {
    std::vector<std::function<void(bool)>> callbacks;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callbacks = connection_callbacks_;
    }
    for (auto& callback : callbacks) {
        callback(connected);
    }
}

void UsbAudio::SetConnectionCallback(std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    connection_callbacks_.clear();
    connection_callbacks_.push_back(std::move(callback));
}

void UsbAudio::AddConnectionCallback(std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    connection_callbacks_.push_back(std::move(callback));
}
