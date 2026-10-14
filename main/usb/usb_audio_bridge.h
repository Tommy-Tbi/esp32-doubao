#pragma once

#include "audio_codec.h"
#include <vector>

// The only owner of codec input/output in the USB-only firmware.
// Raw 48 kHz mono PCM goes directly to/from UAC2: no assistant state, VAD,
// wake-word consumer, network encoder, or competing I2S reader.
class UsbAudioBridge {
public:
    bool Initialize(AudioCodec* codec);
    bool Start();

private:
    static constexpr size_t kBlockSamples = 480;  // 10 ms @ 48 kHz
    AudioCodec* codec_ = nullptr;
    bool started_ = false;
    bool failed_ = false;
    void CaptureTask();
    void PlaybackTask();
    bool CaptureBlock(std::vector<int16_t>& pcm);
    void PlaybackBlock(std::vector<int16_t>& pcm);
};
