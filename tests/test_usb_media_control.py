"""Host test of the HID click queue; no SDK/device required.

python tests/test_usb_media_control.py [--cxx clang++|g++] [--run-dir LOCAL_PATH]
Does not validate USB descriptors/enumeration or the physical touch-pad mapping.
"""
from pathlib import Path
import argparse
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include "usb/usb_media_control.h"
#include <cassert>
#include <vector>
int main() {
    UsbMediaControl media;
    std::vector<uint16_t> reports;
    bool succeed = true;
    auto send = [&](uint16_t usage) {
        if (!succeed) return false;
        reports.push_back(usage);
        return true;
    };
    assert(media.Request());
    assert(!media.Request());  // bounded queue, duplicate not accepted
    media.Poll(true, false, send);
    assert(reports.empty());  // busy endpoint must not consume the click
    succeed = false;
    media.Poll(true, true, send);
    assert(reports.empty());
    succeed = true;
    media.Poll(true, true, send);
    assert(reports == std::vector<uint16_t>{0xCD});
    assert(media.Request());  // next click while release is outstanding
    succeed = false;
    media.Poll(true, true, send);  // failed release must be retried first
    succeed = true;
    media.Poll(true, true, send);
    media.Poll(true, true, send);
    media.Poll(true, true, send);
    media.Poll(true, true, send);
    assert((reports == std::vector<uint16_t>{0xCD, 0, 0xCD, 0}));
    assert(media.Request());
    media.Poll(false, false, send);  // no stale click after reconnect
    media.Poll(true, true, send);
    assert(reports.size() == 4);
    assert(media.Request());
    media.Poll(true, true, send);
    media.Reset();  // lifecycle join/detach discards release state
    media.Poll(true, true, send);
    assert(reports.size() == 5);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++") or shutil.which("g++"))
    parser.add_argument("--run-dir", type=Path)
    args = parser.parse_args()
    if not args.cxx:
        parser.error("a C++17 compiler is required")
    out = ROOT / ".tmp" / "usb_media_control_test"
    out.mkdir(parents=True, exist_ok=True)
    source = out / "test.cc"
    source.write_text(TEST, encoding="utf-8")
    binary = out / "test.exe"
    subprocess.run([args.cxx, "-std=c++17", "-UNDEBUG", "-I" + str(ROOT / "main"),
                    str(source), "-o", str(binary)], check=True)
    if args.run_dir:
        args.run_dir.mkdir(parents=True, exist_ok=True)
        target = args.run_dir.resolve() / binary.name
        if target != binary:
            shutil.copyfile(binary, target)
        binary = target
    subprocess.run([str(binary)], check=True)
    print("USB media control tests passed")


if __name__ == "__main__":
    main()
