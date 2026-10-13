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

    // 必须在所有 TouchButton 对象创建完成之后调用一次。
    // ESP-IDF 要求 touch_element_start() 晚于所有 touch element 的创建，
    // 否则 inited_channel_mask 仍为 0，start 返回 ESP_ERR_INVALID_STATE，
    // 触摸扫描定时器不会启动，所有触摸键都没有任何事件。
    // 另外阈值在 start 时按当时的基线计算（threshold = sens * 0.8 * baseline），
    // 所以 StartAll() 还应放在所有外设初始化完成之后。
    static void StartAll();

    // 启动后延迟重新采样基线（外设稳定后重算阈值），可选调用
    static void ReBaseline(uint32_t delay_ms = 3000);

private:
    touch_button_handle_t touch_handle_ = nullptr;
    std::function<void()> on_press_;
    std::function<void()> on_release_;
    std::function<void()> on_long_press_;
};

#endif // BUTTON_H_
