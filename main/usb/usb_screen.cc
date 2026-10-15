#include "usb_screen.h"
#include "usb_screen_protocol.h"
#include <mutex>
#include <esp_heap_caps.h>
#include "tusb.h"

static_assert(CFG_TUD_VENDOR_RX_BUFSIZE == 64);
static_assert(CFG_TUD_VENDOR_EPSIZE == 64);

namespace {
uint8_t* rx = nullptr;
uint8_t* pending = nullptr;
UsbScreenParser* parser = nullptr;
UsbScreenParser::Rect rect{};
std::mutex mutex;
bool ready = false;
}

bool UsbScreen::Initialize() {
    if (parser) return true;
    rx = static_cast<uint8_t*>(heap_caps_malloc(UsbScreenParser::kBytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    pending = static_cast<uint8_t*>(heap_caps_malloc(UsbScreenParser::kBytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    if (!rx || !pending) {
        free(rx); free(pending); rx = pending = nullptr;
        return false;
    }
    parser = new UsbScreenParser(rx);
    return true;
}

void UsbScreen::Reset() {
    if (parser) parser->Reset();
    std::lock_guard<std::mutex> lock(mutex);
    ready = false;
}

bool UsbScreen::TakeFrame(uint8_t* canvas) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!ready || !canvas) return false;
    for (size_t y = 0; y < rect.height; ++y) {
        memcpy(canvas + ((rect.y + y) * 240 + rect.x) * 2,
            pending + y * rect.width * 2, rect.width * 2);
    }
    ready = false;
    return true;
}

extern "C" void tud_vendor_rx_cb(uint8_t itf, const uint8_t*, uint16_t packet_size) {
    // Keep RX FIFO equal to one FS packet so short packets remain observable.
    uint8_t packet[64];
    do {
        const size_t n = tud_vendor_n_read(itf, packet, sizeof(packet));
        if (parser) {
            parser->Feed(packet, n, [](UsbScreenParser::Rect r, const uint8_t* data, size_t bytes) {
                // Never wait for UI rendering in the audio/USB service task.
                std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
                if (!lock.owns_lock()) return;
                // Preserve rectangle order; drop new frames rather than overwriting pending data.
                if (ready) return;
                memcpy(pending, data, bytes);
                rect = r;
                ready = true;
            });
        }
    } while (tud_vendor_n_available(itf));
    if (parser && packet_size < sizeof(packet)) parser->EndTransfer();
}
