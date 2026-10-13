#ifndef SPARKBOT_KEYS_H_
#define SPARKBOT_KEYS_H_

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

/*
 * 本设备的按键是 GPIO 输入（GPIO1/GPIO2/GPIO3，原理图 TOUCH_BTN1/2/3），不是
 * ESP32 内部电容触摸通道（WROOM-1 模块未引出 TP），详见 config.h 的说明。
 * 按键创建/事件分发使用 main/boards/common/button.h 的 Button（espressif/button 的 GPIO 驱动）。
 * 本文件只负责“同时按两个按键”的组合键判定。
 */

// 组合键统一编号
enum ComboKeyId : int {
    COMBO_KEY_1 = 0,
    COMBO_KEY_2,
    COMBO_KEY_3,
    COMBO_KEY_4,
    COMBO_KEY_BOOT,
};

const char* ComboKeyName(int key_id);

class ComboKeyDetector {
public:
    static constexpr uint32_t kDefaultWindowMs = 250;

    void SetWindowMs(uint32_t window_ms) { window_ms_ = window_ms; }
    uint32_t WindowMs() const { return window_ms_; }

    void OnCombo(std::function<void(uint32_t)> callback) { on_combo_ = std::move(callback); }

    void PressDown(int key_id);
    void PressUp(int key_id);
    void Reset();

    bool IsComboActive() const;

    static std::string Describe(uint32_t mask);

private:
    static constexpr uint32_t kComboLatchBit = 1u << 31;  // 组合键生效标记

    mutable std::mutex mutex_;
    uint32_t pressed_mask_ = 0;
    uint32_t window_ms_ = kDefaultWindowMs;
    int last_key_ = -1;
    int64_t last_press_us_ = 0;
    std::function<void(uint32_t)> on_combo_;
};

#endif  // SPARKBOT_KEYS_H_