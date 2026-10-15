#pragma once
#include <algorithm>
#include <cstdint>

// Caller serializes all events. Time is monotonic microseconds.
class BrightnessKeys {
public:
    enum Action { None = 0, Media = 1, Refresh = 2, Closed = 4 };
    static constexpr int64_t kHoldUs = 2000000;
    static constexpr int64_t kIdleUs = 5000000;
    void Initialize(int value) { brightness_ = std::clamp(value, 10, 100); }
    bool Editing() const { return editing_; }
    bool IsDown(int id) const { return (pressed_ & (1u << id)) != 0; }
    int Value() const { return brightness_; }
    int Poll(int64_t now) {
        if ((pressed_ & 1) && !top_used_ && now - top_down_ >= kHoldUs) {
            top_used_ = true;
            editing_ = !editing_;
            last_event_ = now;
            return editing_ ? Refresh : Closed;
        }
        // Do not let a held key turn into a volume/media action at timeout.
        if (editing_ && !pressed_ && now - last_event_ >= kIdleUs) {
            editing_ = false;
            return Closed;
        }
        return None;
    }
    int Key(int id, bool down, int64_t now) {
        if (id < 0 || id > 2) return None;
        const unsigned bit = 1u << id;
        if (bool(pressed_ & bit) == down) return None;
        int action = Poll(now); // A release at 2s is still a long press.
        last_event_ = now;
        if (down) {
            pressed_ |= bit;
            if (id == 0) { top_down_ = now; top_used_ = (pressed_ & 6) != 0; }
            else if (pressed_ & 1) top_used_ = true;
        } else {
            pressed_ &= ~bit;
            if (id == 0 && !top_used_) {
                if (editing_) { editing_ = false; action |= Closed; }
                else action |= Media;
            } else if (id != 0 && editing_) {
                brightness_ = std::clamp(brightness_ + (id == 1 ? -10 : 10), 10, 100);
                action |= Refresh;
            }
        }
        return action;
    }
private:
    int brightness_ = 75;
    unsigned pressed_ = 0;
    bool editing_ = false, top_used_ = false;
    int64_t top_down_ = 0, last_event_ = 0;
};
