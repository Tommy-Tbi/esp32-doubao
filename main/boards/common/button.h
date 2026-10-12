#ifndef BUTTON_H_
#define BUTTON_H_

#include <driver/gpio.h>
#include <driver/touch_sensor.h>
#include <iot_button.h>
#include <button_types.h>
#include <button_adc.h>
#include <button_gpio.h>
#include <touch_element/touch_button.h>
#include <functional>

class Button {
public:
    Button(button_handle_t button_handle);
    Button(gpio_num_t gpio_num, bool active_high = false, uint16_t long_press_time = 0, uint16_t short_press_time = 0, bool enable_power_save = false);
    ~Button();

    void OnPressDown(std::function<void()> callback);
    void OnPressUp(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback);
    void OnClick(std::function<void()> callback);
    void OnDoubleClick(std::function<void()> callback);
    void OnMultipleClick(std::function<void()> callback, uint8_t click_count = 3);

protected:
    gpio_num_t gpio_num_;
    button_handle_t button_handle_ = nullptr;

    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::function<void()> on_multiple_click_;
};

#if CONFIG_SOC_ADC_SUPPORTED
class AdcButton : public Button {
public:
    AdcButton(const button_adc_config_t& adc_config);
};
#endif

class PowerSaveButton : public Button {
public:
    PowerSaveButton(gpio_num_t gpio_num) : Button(gpio_num, false, 0, 0, true) {
    }
};

// Touch button support
class TouchButton {
public:
    TouchButton(int touch_pad, float sensitivity = 0.1F);
    ~TouchButton();

    void OnPress(std::function<void()> callback);
    void OnRelease(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback, uint32_t threshold_ms = 2000);

private:
    touch_button_handle_t touch_handle_ = nullptr;
    std::function<void()> on_press_;
    std::function<void()> on_release_;
    std::function<void()> on_long_press_;
};

#endif // BUTTON_H_
