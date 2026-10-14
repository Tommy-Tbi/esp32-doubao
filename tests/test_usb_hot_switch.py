"""Compile/run real USB mode and key-chord code against small host SDK fakes.

Run: python tests/test_usb_hot_switch.py [--cxx clang++|g++] [--run-dir LOCAL_PATH]
Needs a C++20 host compiler. Does NOT replace the README board acceptance test.
Generated headers/executable stay under the project's .tmp directory.
"""
from pathlib import Path
import argparse
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

TEST = r'''
#include "usb_hot_switch_fixture.h"
#define private public
#include "boards/esp-sparkbot/otg_mode.cc"
#undef TAG
#include "boards/esp-sparkbot/sparkbot_keys.cc"
#undef private

int main() {
    auto& mode = OtgMode::GetInstance();
    auto& usb = UsbAudio::GetInstance();
    Display display;
    mode.display_ = &display;
    auto reset = [&] {
        native_phy = nullptr;
        owner.clear();
        boot_native = notifications = phy_installs = 0;
        fail_install = fail_release = fail_save = false;
        usb.initialized = usb.connected = false;
        usb.fail_resume = usb.fail_uninstall = false;
        mode.native_ = mode.switching_ = false;
        mode.faulted_ = false;
    };
    auto boot = [&] {
        try { mode.Run(); } catch (const StopLoop&) {}
    };

    // Boot and twenty bidirectional handoffs without a cable event/reboot.
    reset(); boot();
    assert(owner == "audio");
    for (int i = 0; i < 20; ++i) {
        mode.SwitchMode();
        assert(mode.native_ && owner == "native" && pad_enabled && boot_native == 1);
        mode.SwitchMode();
        assert(!mode.native_ && owner == "audio" && boot_native == 0);
    }
    // Native-mode cold boot must support returning to audio too.
    reset(); boot_native = 1; boot();
    assert(mode.native_ && owner == "native");
    mode.SwitchMode(); assert(owner == "audio");

    // Failed OTG teardown must never acquire another PHY.
    reset(); boot(); usb.fail_uninstall = true;
    mode.SwitchMode();
    assert(mode.faulted_ && owner == "audio" && phy_installs == 0);

    // Native install failure rolls back to audio; does not persist native mode.
    reset(); boot(); fail_install = true;
    mode.SwitchMode();
    assert(!mode.faulted_ && !mode.native_ && owner == "audio" && boot_native == 0);

    // Failed native release leaves its handle/pads usable; no second owner.
    reset(); boot_native = 1; boot(); fail_release = true;
    mode.SwitchMode();
    assert(!mode.faulted_ && mode.native_ && owner == "native" && pad_enabled);

    // Failed audio restore stops instead of rebooting/reusing a partial stack.
    reset(); boot_native = 1; boot(); usb.fail_resume = true;
    mode.SwitchMode();
    assert(mode.faulted_ && owner.empty() && boot_native == 1);

    // Persistence failure must not undo an otherwise successful live handoff.
    reset(); boot(); fail_save = true;
    mode.SwitchMode();
    assert(mode.native_ && owner == "native" && !mode.faulted_);
    assert(display.notice.find("保存失败") != std::string::npos);

    // Duplicate requests while switching are ignored.
    reset(); mode.task_ = reinterpret_cast<void*>(1);
    mode.ToggleByKeys(3); mode.ToggleByKeys(3);
    assert(notifications == 1);

    // A chord triggers once until ALL keys are released, including third keys.
    ComboKeyDetector keys;
    int chords = 0;
    keys.OnCombo([&](uint32_t) { ++chords; });
    keys.PressDown(COMBO_KEY_1); keys.PressDown(COMBO_KEY_2);
    keys.PressDown(COMBO_KEY_BOOT);
    keys.PressUp(COMBO_KEY_2); keys.PressDown(COMBO_KEY_2);
    assert(chords == 1);
    keys.PressUp(COMBO_KEY_1); keys.PressUp(COMBO_KEY_2); keys.PressUp(COMBO_KEY_BOOT);
    keys.PressDown(COMBO_KEY_1); keys.PressDown(COMBO_KEY_2);
    assert(chords == 2);
    std::puts("USB mode host tests passed (20 round trips + failure/chord cases)");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++") or shutil.which("g++"))
    parser.add_argument("--run-dir", type=Path,
                        help="copy executable here if the source share disallows execution")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("a C++20 compiler is required")
    out = ROOT / ".tmp" / "usb_hot_switch_test"
    out.mkdir(parents=True, exist_ok=True)
    for name in ("esp_log.h", "lvgl.h", "nvs_flash.h", "usb_audio.h", "display.h",
                 "esp_timer.h", "freertos/FreeRTOS.h", "freertos/task.h",
                 "esp_private/usb_phy.h", "esp_private/periph_ctrl.h",
                 "hal/usb_serial_jtag_ll.h"):
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('#pragma once\n#include "usb_hot_switch_fixture.h"\n', encoding="utf-8")
    source = out / "test.cc"
    source.write_text(TEST, encoding="utf-8")
    binary = out / "test.exe"
    subprocess.run([args.cxx, "-std=c++20", "-finput-charset=UTF-8", "-UNDEBUG",
                    "-I" + str(out), "-I" + str(ROOT / "tests"),
                    "-I" + str(ROOT / "main"), str(source), "-o", str(binary)], check=True)
    if args.run_dir:
        args.run_dir.mkdir(parents=True, exist_ok=True)
        run_binary = args.run_dir.resolve() / binary.name
        if run_binary != binary:
            shutil.copyfile(binary, run_binary)
        binary = run_binary
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
