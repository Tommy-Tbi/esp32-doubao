#include "sparkbot_keys.h"

#include <esp_log.h>
#include <esp_timer.h>

#define TAG "SparkBotKeys"

// ---------------------------------------------------------------------------
// 组合键：同时按两个按键
// ---------------------------------------------------------------------------

const char* ComboKeyName(int key_id) {
    switch (key_id) {
        case COMBO_KEY_1:    return "KEY1";
        case COMBO_KEY_2:    return "KEY2";
        case COMBO_KEY_3:    return "KEY3";
        case COMBO_KEY_4:    return "KEY4";
        case COMBO_KEY_BOOT: return "BOOT";
        default:             return "?";
    }
}

std::string ComboKeyDetector::Describe(uint32_t mask) {
    std::string out;
    for (int i = 0; i < 31; i++) {
        if (mask & (1u << i)) {
            if (!out.empty()) out += "+";
            out += ComboKeyName(i);
        }
    }
    return out.empty() ? std::string("NONE") : out;
}

void ComboKeyDetector::PressDown(int key_id) {
    if (key_id < 0 || key_id >= 31) {
        return;
    }

    uint32_t combo_mask = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint32_t bit = 1u << key_id;
        if (pressed_mask_ & bit) {
            return;  // 已经在按下状态，忽略重复事件
        }

        const int held = __builtin_popcount(pressed_mask_ & ~kComboLatchBit);
        const int64_t now = esp_timer_get_time();
        pressed_mask_ |= bit;

        // 条件一：第二个键按下时第一个键仍按着（真正的同时按）
        const bool overlap = held >= 1;
        // 条件二：另一个键在窗口时间内被按下（快速先后按两个不同键）
        const bool quick = (last_key_ >= 0 && last_key_ != key_id) &&
                           (now - last_press_us_) <= static_cast<int64_t>(window_ms_) * 1000;
        // One handoff per chord. A third key or a re-press while another key
        // remains held must not immediately switch back to the previous mode.
        if ((overlap || quick) && !(pressed_mask_ & kComboLatchBit)) {
            combo_mask = pressed_mask_ & ~kComboLatchBit;
            pressed_mask_ |= kComboLatchBit;
        }
        last_key_ = key_id;
        last_press_us_ = now;
    }

    if (combo_mask != 0) {
        ESP_LOGI(TAG, "combo keys: %s", Describe(combo_mask).c_str());
        if (on_combo_) {
            on_combo_(combo_mask);
        }
    }
}

void ComboKeyDetector::PressUp(int key_id) {
    if (key_id < 0 || key_id >= 31) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pressed_mask_ &= ~(1u << key_id);
    if ((pressed_mask_ & ~kComboLatchBit) == 0) {
        pressed_mask_ = 0;  // 全部松开，解除组合键标记
        last_key_ = -1;
        last_press_us_ = 0;
    }
}

void ComboKeyDetector::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pressed_mask_ = 0;
    last_key_ = -1;
    last_press_us_ = 0;
}

bool ComboKeyDetector::IsComboActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (pressed_mask_ & kComboLatchBit) != 0;
}
