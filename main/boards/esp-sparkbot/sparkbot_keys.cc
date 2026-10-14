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
    if (key_id < 0 || key_id >= 31) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t bit = 1u << key_id;
    if (pressed_mask_ & bit) return;
    const bool was_pair = (pressed_mask_ & kModeKeys) == kModeKeys;
    pressed_mask_ |= bit;
    if (!was_pair && (pressed_mask_ & kModeKeys) == kModeKeys && !fired_) {
        chord_ = true;
        deadline_us_ = esp_timer_get_time() + kHoldUs;
    }
}

void ComboKeyDetector::PressUp(int key_id) {
    if (key_id < 0 || key_id >= 31) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pressed_mask_ &= ~(1u << key_id);
    if ((pressed_mask_ & kModeKeys) != kModeKeys) deadline_us_ = 0;
    if ((pressed_mask_ & kModeKeys) == 0) {
        chord_ = false;
        fired_ = false;
    }
}

void ComboKeyDetector::Poll() {
    std::function<void(uint32_t)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!deadline_us_ || fired_ || esp_timer_get_time() < deadline_us_) return;
        fired_ = true;
        deadline_us_ = 0;
        callback = on_combo_;
    }
    if (callback) callback(kModeKeys);
}

void ComboKeyDetector::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pressed_mask_ = 0;
    deadline_us_ = 0;
    chord_ = fired_ = false;
}

bool ComboKeyDetector::IsComboActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chord_;
}
