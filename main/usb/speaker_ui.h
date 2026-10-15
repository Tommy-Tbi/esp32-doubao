#pragma once

// Speaker wave count is a setting indicator, not the live PCM meter.
constexpr int SpeakerWaveCount(int volume) {
    return volume <= 0 ? 0 : volume <= 33 ? 1 : volume <= 66 ? 2 : 3;
}
