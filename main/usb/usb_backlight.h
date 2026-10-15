#pragma once
#include <algorithm>
#include <driver/ledc.h>
#include <nvs.h>
#include <esp_err.h>

// Same 25 kHz / 10-bit LEDC technique as boards/common/backlight.cc, but
// without its hidden 25% restore scaling or asynchronous transition timer.
// Sole owner of GPIO46: the codec MUST be constructed with GPIO_NUM_NC for PA.
class UsbBacklight {
public:
    void Initialize(gpio_num_t pin) {
        nvs_handle_t handle;
        int32_t value = 75;
        // Separate key: legacy display/brightness was scaled at restore time.
        if (nvs_open("display", NVS_READONLY, &handle) == ESP_OK) {
            nvs_get_i32(handle, "usb_brightness", &value);
            nvs_close(handle);
        }
        saved_ = std::clamp(static_cast<int>(value), 10, 100);
        const ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 25000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer));
        const ledc_channel_config_t channel = {
            .gpio_num = pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            // Start at maximum duty while shared PA control settles at boot.
            .duty = 1023,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }
    int Saved() const { return saved_; }
    void Set(int percent) {
        const uint32_t duty = 1023u * std::clamp(percent, 10, 100) / 100;
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    }
    bool Save(int percent) {
        percent = std::clamp(percent, 10, 100);
        if (percent == saved_) return true;
        nvs_handle_t handle;
        if (nvs_open("display", NVS_READWRITE, &handle) != ESP_OK) return false;
        esp_err_t err = nvs_set_i32(handle, "usb_brightness", percent);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
        if (err == ESP_OK) saved_ = percent;
        return err == ESP_OK;
    }
private:
    int saved_ = 75;
};
