#pragma once
#include <cstdint>

namespace UsbScreen {
bool Initialize();
// Called only by the USB task, or after it has been joined.
void Reset();
// Copies a completed rectangle into a persistent 240x240 native RGB565 canvas.
// Called by the UI task, never writes to the LCD from USB callbacks.
bool TakeFrame(uint8_t* canvas);
}
