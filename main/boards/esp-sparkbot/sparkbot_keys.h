#ifndef SPARKBOT_KEYS_H_
#define SPARKBOT_KEYS_H_

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

enum ComboKeyId : int {
    COMBO_KEY_1 = 0,
    COMBO_KEY_2,
    COMBO_KEY_3,
    COMBO_KEY_4,
    COMBO_KEY_BOOT,
};

const char* ComboKeyName(int key_id);

// Left/right volume touch pads (2/3), continuously held for three seconds.
// Call Poll regularly from a task; callbacks run outside the state lock.
class ComboKeyDetector {
public:
    static constexpr uint32_t kModeKeys = (1u << COMBO_KEY_2) | (1u << COMBO_KEY_3);
    static constexpr int64_t kHoldUs = 3000000;
    void OnCombo(std::function<void(uint32_t)> callback) { on_combo_ = std::move(callback); }
    void PressDown(int key_id);
    void PressUp(int key_id);
    void Poll();
    void Reset();
    bool IsComboActive() const;
    static std::string Describe(uint32_t mask);

private:
    mutable std::mutex mutex_;
    uint32_t pressed_mask_ = 0;
    int64_t deadline_us_ = 0;
    bool chord_ = false;
    bool fired_ = false;
    std::function<void(uint32_t)> on_combo_;
};
#endif
