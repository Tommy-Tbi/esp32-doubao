#pragma once

#include <atomic>
#include <cstdint>

// Producers only request a click; the USB task owns HID I/O and release state.
class UsbMediaControl {
public:
    static constexpr uint16_t kPlayPause = 0x00CD;  // Consumer Play/Pause

    bool Request() {
        return !pending_.exchange(true);
    }

    // USB-task only. Failed/busy transfers are retried, release always comes first.
    template <typename Send>
    void Poll(bool connected, bool ready, Send send) {
        if (!connected) {
            Reset();
            return;
        }
        if (!ready) return;
        if (release_) {
            if (send(uint16_t{0})) release_ = false;
        } else if (pending_.load() && send(kPlayPause)) {
            pending_ = false;
            release_ = true;
        }
    }

    // Only before task start, after task join, or from the USB task itself.
    void Reset() {
        pending_ = false;
        release_ = false;
    }

private:
    std::atomic<bool> pending_{false};
    bool release_ = false;
};
