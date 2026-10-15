"""Host-only XFZ1986 v2 parser tests; no USB/LCD hardware validation."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "usb/usb_screen_protocol.h"
#include <cassert>
#include <vector>

std::vector<uint8_t> Frame(unsigned w, unsigned h, unsigned type = 0) {
    const unsigned bytes = w * h * 2;
    std::vector<uint8_t> f(16 + bytes, 0x5a);
    for (int i=0; i<16; ++i) f[i]=0;
    f[2] = type; f[8] = w; f[9] = w >> 8; f[10] = h; f[11] = h >> 8;
    const uint32_t packed = (bytes << 10) | 1023;
    for (int i=0; i<4; ++i) f[12+i] = packed >> (8*i);
    return f;
}
int main() {
    uint8_t buffer[UsbScreenParser::kBytes];
    UsbScreenParser p(buffer);
    int frames = 0;
    auto sink = [&](UsbScreenParser::Rect r, const uint8_t* data, size_t bytes) {
        ++frames;
        assert(bytes == size_t(r.width)*r.height*2);
        for(size_t i=0;i<bytes;++i) assert(data[i] == 0x5a);
    };
    auto full = Frame(240,240);
    // Fragmented headers, all packet offsets, exact payload length.
    for (size_t chunk : {size_t(1), size_t(7), size_t(64), size_t(511)}) {
        int before = frames;
        for (size_t i=0; i<full.size(); i+=chunk)
            p.Feed(full.data()+i, std::min(chunk, full.size()-i), sink);
        assert(frames == before+1);
    }
    // Back-to-back frames; valid subrectangle; no dependency on a ZLP.
    auto small = Frame(4,6); small[4]=236; small[6]=234;
    p.Feed(small.data(), small.size(), sink);
    assert(frames == 5);
    // Out of bounds, unsupported encoding, wrong length: drain, never publish.
    small[4]=237;
    p.Feed(small.data(), small.size(), sink);
    auto jpeg = Frame(240,240,3);
    p.Feed(jpeg.data(), jpeg.size(), sink);
    auto bad = Frame(4,6); bad[8]=3;
    p.Feed(bad.data(),bad.size(),sink);
    assert(frames == 5);
    // Zero-length Windows terminator and Linux ZLP.
    uint8_t end[16] = {}; end[2]=0xff;
    p.Feed(end,16,sink); p.EndTransfer();
    // Truncated transfer and disconnect clear both header and payload state.
    p.Feed(full.data(),100,sink); p.EndTransfer();
    p.Feed(full.data(),3,sink); p.Reset();
    p.Feed(full.data(),full.size(),sink);
    assert(frames == 6);
    // Maximum 22-bit payload must not overflow the frame buffer.
    uint8_t oversized[16] = {}; oversized[8]=240; oversized[10]=240;
    oversized[12]=oversized[13]=oversized[14]=oversized[15]=255;
    p.Feed(oversized,16,sink);
    std::vector<uint8_t> garbage((1u<<22)-1,0);
    p.Feed(garbage.data(),garbage.size(),sink);
    p.Feed(full.data(),full.size(),sink);
    assert(frames == 7);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++') or shutil.which('g++'))
    parser.add_argument('--run-dir', type=Path)
    args = parser.parse_args()
    if not args.cxx:
        parser.error('C++17 compiler required')
    out = ROOT / '.tmp' / 'usb_screen_test'
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'test.cc'
    source.write_text(SOURCE, encoding='utf-8')
    binary = out / 'usb_screen_test.exe'
    subprocess.run([args.cxx, '-std=c++17', '-UNDEBUG', '-I'+str(ROOT/'main'),
                    str(source), '-o', str(binary)], check=True)
    if args.run_dir:
        args.run_dir.mkdir(parents=True, exist_ok=True)
        target = args.run_dir.resolve() / binary.name
        if target != binary:
            shutil.copyfile(binary, target)
        binary = target
    subprocess.run([str(binary)], check=True)
    print('USB screen parser tests passed')


if __name__ == '__main__':
    main()
