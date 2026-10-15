#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Peak envelope in dBFS, -60..0 dB mapped to 0..100. Instant attack,
// 300 ms release at the bridge's 10 ms block cadence. Not a calibrated SPL meter.
class AudioLevel {
public:
    void Reset() { envelope_ = 0; }
    int Update(const int16_t* pcm, size_t count) {
        int peak = 0;
        for (size_t i = 0; i < count; ++i)
            peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
        const float level = peak ? std::clamp(
            (20.0f * std::log10(peak / 32768.0f) + 60.0f) / 60.0f * 100.0f,
            0.0f, 100.0f) : 0.0f;
        envelope_ = std::max(level, envelope_ - 100.0f / 30.0f);
        return static_cast<int>(envelope_);
    }
private:
    float envelope_ = 0;
};
