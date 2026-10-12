#include "button.h"

#include <button_gpio.h>
#include <esp_log.h>
#include "touch_element/touch_button.h"

#define TAG "Button"

#if CONFIG_SOC_ADC_SUPPORTED
AdcButton::AdcButton(const button_adc_config_t& adc_config) : Button(nullptr) {
    button_config_t btn_config = {
        .long_press_time = 2000,
        .short_press_time = 0,
    };
    ESP_ERROR_CHECK(iot_button_new_adc_device(&btn_config, &adc_config, &button_handle_));
}
#endif

Button::Button(button_handle_t button_handle) : button_handle_(button_handle) {
}

Button::Button(gpio_num_t gpio_num, bool active_high, uint16_t long_press_time, uint16_t short_press_time, bool enable_power_save) : gpio_num_(gpio_num) {
    if (gpio_num == GPIO_NUM_NC) {
        return;
    }
    button_config_t button_config = {
        .long_press_time = long_press_time,
        .short_press_time = short_press_time
    };
    button_gpio_config_t gpio_config = {
        .gpio_num = gpio_num,
        .active_level = static_cast<uint8_t>(active_high ? 1 : 0),
        .enable_power_save = enable_power_save,
        .disable_pull = false
    };
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&button_config, &gpio_config, &button_handle_));
}

Button::~Button() {
    if (button_handle_ != NULL) {
        iot_button_delete(button_handle_);
    }
}

void Button::OnPressDown(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_press_down_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_PRESS_DOWN, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->on_press_down_) {
            button->on_press_down_();
        }
    }, this);
}

void Button::OnPressUp(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_press_up_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_PRESS_UP, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->on_press_up_) {
            button->on_press_up_();
        }
    }, this);
}

void Button::OnLongPress(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_long_press_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_LONG_PRESS_START, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->on_long_press_) {
            button->on_long_press_();
        }
    }, this);
}

void Button::OnClick(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_click_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_SINGLE_CLICK, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->on_click_) {
            button->on_click_();
        }
    }, this);
}

void Button::OnDoubleClick(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_double_click_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_DOUBLE_CLICK, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->on_double_click_) {
            button->on_double_click_();
        }
    }, this);
}

void Button::OnMultipleClick(std::function<void()> callback, uint8_t click_count) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_multiple_click_ = callback;
    button_event_args_t event_args = {
        .multiple_clicks = {
            .clicks = click_count
        }
    };
    iot_button_register_cb(button_handle_, BUTTON_MULTIPLE_CLICK, &event_args, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->on_multiple_click_) {
            button->on_multiple_click_();
        }
    }, this);
}

// TouchButton implementation
TouchButton::TouchButton(int touch_pad, float sensitivity) {
    static bool touch_initialized = false;

    // Initialize touch element library once
    if (!touch_initialized) {
        touch_elem_global_config_t global_config = TOUCH_ELEM_GLOBAL_DEFAULT_CONFIG();
        ESP_ERROR_CHECK(touch_element_install(&global_config));
        ESP_LOGI(TAG, "Touch element library installed");

        touch_button_global_config_t button_global_config = TOUCH_BUTTON_GLOBAL_DEFAULT_CONFIG();
        ESP_ERROR_CHECK(touch_button_install(&button_global_config));
        ESP_LOGI(TAG, "Touch button installed");

        touch_element_start();
        ESP_LOGI(TAG, "Touch element library started");

        touch_initialized = true;
    }

    // Create touch button
    touch_button_config_t button_config = {
        .channel_num = (touch_pad_t)touch_pad,
        .channel_sens = sensitivity
    };
    ESP_ERROR_CHECK(touch_button_create(&button_config, &touch_handle_));

    // Subscribe to events with 'this' as user data
    ESP_ERROR_CHECK(touch_button_subscribe_event(touch_handle_,
        TOUCH_ELEM_EVENT_ON_PRESS | TOUCH_ELEM_EVENT_ON_RELEASE | TOUCH_ELEM_EVENT_ON_LONGPRESS,
        this));

    // Set callback dispatch method
    ESP_ERROR_CHECK(touch_button_set_dispatch_method(touch_handle_, TOUCH_ELEM_DISP_CALLBACK));

    // Set the callback
    ESP_ERROR_CHECK(touch_button_set_callback(touch_handle_, [](touch_button_handle_t handle, touch_button_message_t* message, void* usr_data) {
        TouchButton* btn = static_cast<TouchButton*>(usr_data);
        if (message->event == TOUCH_BUTTON_EVT_ON_PRESS && btn->on_press_) {
            btn->on_press_();
        } else if (message->event == TOUCH_BUTTON_EVT_ON_RELEASE && btn->on_release_) {
            btn->on_release_();
        } else if (message->event == TOUCH_BUTTON_EVT_ON_LONGPRESS && btn->on_long_press_) {
            btn->on_long_press_();
        }
    }));
}

TouchButton::~TouchButton() {
    if (touch_handle_ != nullptr) {
        touch_button_delete(touch_handle_);
    }
}

void TouchButton::OnPress(std::function<void()> callback) {
    on_press_ = callback;
}

void TouchButton::OnRelease(std::function<void()> callback) {
    on_release_ = callback;
}

void TouchButton::OnLongPress(std::function<void()> callback, uint32_t threshold_ms) {
    on_long_press_ = callback;
    if (touch_handle_ != nullptr) {
        touch_button_set_longpress(touch_handle_, threshold_ms);
    }
}