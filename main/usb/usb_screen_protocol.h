#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// XFZ1986 v2: 16-byte little-endian header (not the stale "20bytes"
// comment in the reference). No packet prefix and no CRC verification in host.
class UsbScreenParser {
public:
    static constexpr size_t kBytes = 240 * 240 * 2;
    struct Rect { uint16_t x, y, width, height; };
    explicit UsbScreenParser(uint8_t* buffer) : buffer_(buffer) {}
    void Reset() { header_used_ = received_ = total_ = 0; valid_ = false; }
    template<class Sink>
    void Feed(const uint8_t* data, size_t size, Sink sink) {
        while (size) {
            if (header_used_ < sizeof(header_)) {
                const size_t n = std::min(size, sizeof(header_) - header_used_);
                memcpy(header_ + header_used_, data, n);
                header_used_ += n; data += n; size -= n;
                if (header_used_ != sizeof(header_)) continue;
                rect_ = {Read16(4), Read16(6), Read16(8), Read16(10)};
                const uint32_t packed = uint32_t(header_[12]) | uint32_t(header_[13]) << 8 |
                    uint32_t(header_[14]) << 16 | uint32_t(header_[15]) << 24;
                total_ = packed >> 10;
                valid_ = buffer_ && header_[2] == 0 && rect_.width && rect_.height &&
                    uint32_t(rect_.x) + rect_.width <= 240 &&
                    uint32_t(rect_.y) + rect_.height <= 240 &&
                    total_ == size_t(rect_.width) * rect_.height * 2;
                // Includes Windows' type=0xff, zero-payload ZLP workaround.
                if (!total_) { Reset(); continue; }
            }
            const size_t n = std::min(size, total_ - received_);
            if (valid_) memcpy(buffer_ + received_, data, n);
            received_ += n; data += n; size -= n;
            if (received_ == total_) {
                if (valid_) sink(rect_, buffer_, total_);
                Reset();
            }
        }
    }
    // A real USB short packet terminates a transfer, including truncated frames.
    void EndTransfer() { Reset(); }
private:
    uint16_t Read16(size_t p) const { return header_[p] | uint16_t(header_[p + 1]) << 8; }
    uint8_t* buffer_;
    uint8_t header_[16] = {};
    size_t header_used_ = 0, received_ = 0, total_ = 0;
    bool valid_ = false;
    Rect rect_{};
};
