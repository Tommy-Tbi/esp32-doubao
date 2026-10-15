"""Compile/run real USB mode, raw PCM bridge and key code against host SDK fakes.

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
#undef TAG
#include "usb/usb_audio_bridge.cc"
#undef private

int main() {
    // Real level-meter implementation: silence, full-scale negative sample,
    // -20 dBFS mapping, monotonic release, and eventual zero.
    AudioLevel meter;
    int16_t sample = 0;
    assert(meter.Update(&sample, 1) == 0);
    sample = INT16_MIN;
    assert(meter.Update(&sample, 1) == 100);
    sample = 0;
    int previous = 100;
    for (int i = 0; i < 35; ++i) {
        int level = meter.Update(&sample, 1);
        assert(level <= previous && level >= 0);
        previous = level;
    }
    assert(previous == 0);
    sample = 3277;
    assert(meter.Update(&sample, 1) >= 66 && meter.Update(&sample, 1) <= 67);

    auto& mode = OtgMode::GetInstance();
    auto& usb = UsbAudio::GetInstance();
    Display display;
    mode.display_ = &display;
    auto reset = [&] {
        native_phy = nullptr;
        owner.clear();
        boot_native = notifications = phy_installs = 0;
        fail_install = fail_release = fail_save = false;
        usb.initialized = usb.connected = usb.mic_active = false;
        usb.fail_resume = usb.fail_uninstall = false;
        mode.native_ = mode.switching_ = false;
        mode.faulted_ = false;
        mode.status_text_ = nullptr;
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

    // Recording (including local mute) must keep USB audio attached.
    reset(); boot(); usb.connected = usb.mic_active = true;
    assert(std::string(mode.SwitchMode()).find("Mic in use") != std::string::npos);
    assert(!mode.native_ && owner == "audio" && phy_installs == 0);
    usb.mic_active = false;
    mode.SwitchMode();
    assert(mode.native_ && owner == "native");

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
    const char* result = mode.SwitchMode();
    assert(mode.native_ && owner == "native" && !mode.faulted_);
    assert(std::string(result).find("save failed") != std::string::npos);

    // Duplicate requests while switching are ignored.
    reset(); mode.task_ = reinterpret_cast<void*>(1);
    mode.ToggleByKeys(3); mode.ToggleByKeys(3);
    assert(notifications == 1);

    // Only the two volume pads form a chord; timing starts at the second pad.
    ComboKeyDetector keys;
    int chords = 0;
    keys.OnCombo([&](uint32_t mask) {
        assert(mask == ComboKeyDetector::kModeKeys);
        ++chords;
    });
    now_us = 0;
    keys.PressDown(COMBO_KEY_1); keys.PressDown(COMBO_KEY_2);
    now_us += 6000000; keys.Poll();
    assert(chords == 0 && !keys.IsComboActive());
    keys.Reset();
    keys.PressDown(COMBO_KEY_2);
    now_us += 6000000;
    keys.PressDown(COMBO_KEY_3);
    const auto start = now_us;
    keys.PressDown(COMBO_KEY_3);  // repeated down must not restart the hold
    keys.PressDown(-1); keys.PressUp(31);  // ignore invalid key ids
    static_assert(ComboKeyDetector::kHoldUs == 3000000);
    now_us = start + 2999999; keys.Poll();
    assert(chords == 0 && keys.IsComboActive());
    now_us = start + 3000000; keys.Poll();
    assert(chords == 1);
    now_us += 9000000; keys.Poll();
    keys.PressDown(COMBO_KEY_BOOT);
    keys.PressUp(COMBO_KEY_2); keys.PressDown(COMBO_KEY_2);
    now_us += 6000000; keys.Poll();
    assert(chords == 1);  // no repeat until BOTH volume pads are released
    keys.PressUp(COMBO_KEY_2); keys.PressUp(COMBO_KEY_3);
    keys.PressUp(COMBO_KEY_BOOT);

    // Early release cancels; pressing again requires a new continuous 3s.
    keys.PressDown(COMBO_KEY_2); keys.PressDown(COMBO_KEY_3);
    now_us += 2000000;
    keys.PressUp(COMBO_KEY_2);
    assert(keys.IsComboActive());  // suppress volume on remaining key release
    now_us += 6000000; keys.Poll(); assert(chords == 1);
    keys.PressDown(COMBO_KEY_2);
    now_us += 2000000;
    keys.PressDown(COMBO_KEY_1);  // unrelated pad must not restart the timer
    now_us += 1000000; keys.Poll(); assert(chords == 2);
    keys.Reset(); now_us += 6000000; keys.Poll();
    assert(chords == 2 && !keys.IsComboActive());
    keys.PressDown(COMBO_KEY_2); keys.PressDown(COMBO_KEY_3);
    now_us += 2999999;
    keys.PressUp(COMBO_KEY_2);
    assert(keys.IsComboActive());
    keys.PressUp(COMBO_KEY_3);
    now_us += 6000000; keys.Poll();
    assert(chords == 2 && !keys.IsComboActive());
    // Raw microphone capture must not need assistant, VAD or wake-word state.
    reset();
    AudioCodec codec;
    UsbAudioBridge bridge;
    assert(bridge.Initialize(&codec) && codec.starts == 1);
    task_creates = 0;
    assert(bridge.Start() && task_creates == 2);
    assert(bridge.Start() && task_creates == 2);  // never spawn another I2S reader
    std::vector<int16_t> pcm(480);
    usb.initialized = usb.connected = true;
    usb.mic_active = false;
    assert(bridge.CaptureBlock(pcm) && codec.reads == 1 && usb.mic_writes == 0);
    usb.mic_active = true;
    assert(bridge.CaptureBlock(pcm) && usb.mic_writes == 1);
    assert(usb.mic_pcm == pcm && usb.mic_pcm[0] == 1000 && usb.mic_pcm[479] == 1479);
    codec.on_read = [&] { usb.mic_active = false; };
    assert(bridge.CaptureBlock(pcm) && usb.mic_writes == 1);  // stop mid-read
    codec.on_read = {};
    usb.connected = false;
    assert(bridge.CaptureBlock(pcm) && codec.reads == 4 && usb.mic_writes == 1);
    usb.connected = usb.mic_active = true;
    assert(bridge.CaptureBlock(pcm) && usb.mic_writes == 2);  // hot return to audio
    codec.fail_read = true;
    assert(!bridge.CaptureBlock(pcm) && usb.mic_writes == 2);  // don't send stale data
    assert(pcm == std::vector<int16_t>(480, 0));
    codec.fail_read = false;
    usb.fail_send = true;
    assert(!bridge.CaptureBlock(pcm));  // ring backpressure is reported
    usb.fail_send = false;
    AudioCodec bad_codec;
    bad_codec.input_rate = 16000;
    UsbAudioBridge invalid_bridge;
    assert(!invalid_bridge.Initialize(&bad_codec) && bad_codec.starts == 0);
    UsbAudioBridge failed_bridge;
    assert(failed_bridge.Initialize(&codec));
    task_creates = 0; fail_task_at = 2;
    assert(!failed_bridge.Start() && task_creates == 2);
    assert(!failed_bridge.Start() && task_creates == 2);  // no retry after partial start
    fail_task_at = -1;

    usb.speaker_pcm.assign(480, 1234);
    bridge.PlaybackBlock(pcm);
    assert(codec.output == std::vector<int16_t>(480, 1234));
    bridge.PlaybackBlock(pcm);
    assert(codec.output == std::vector<int16_t>(480, 0));  // underrun -> silence
    usb.speaker_pcm.assign(480, 1234);
    usb.on_fetch = [&] { usb.connected = false; };
    bridge.PlaybackBlock(pcm);
    assert(codec.output == std::vector<int16_t>(480, 0));  // detach mid-fetch
    usb.on_fetch = {};

    // The mode manager sends state only; UsbDisplay owns the single view.
    mode.SetUsbStatus("USB 音频");
    assert(display.status == "USB 音频");
    mode.SetUsbStatus("切换到调试模式");
    assert(display.status == "切换到调试模式");
    mode.SetUsbStatus("切换到音频模式");
    assert(display.status == "切换到音频模式");
    mode.SetUsbStatus("USB 异常");
    assert(display.status == "USB 异常");
    mode.SetUsbStatus("USB 调试");
    assert(display.status == "USB 调试");
    mode.SetUsbStatus("");
    assert(display.status.empty());

    // Duplicate status refreshes must not erase the live result notification.
    mode.status_text_ = nullptr;
    mode.SetUsbStatus("USB 调试");
    display.ShowNotification("切换完成", 3500);
    int updates = display.status_updates;
    mode.SetUsbStatus("USB 调试");
    assert(display.notice == "切换完成" && display.status_updates == updates);
    std::puts("USB host tests passed (20 round trips + raw mic/playback + failures/chords/UI)");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++") or shutil.which("g++"))
    parser.add_argument("--compile-only", action="store_true",
                        help="compile without executing (for no-execute shares)")
    parser.add_argument("--run-dir", type=Path,
                        help="copy executable here if the source share disallows execution")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("a C++20 compiler is required")
    firmware = (ROOT / "main/usb/firmware.cmake").read_text(encoding="utf-8")
    for forbidden in ("application.cc", "audio_service.cc", "protocols/", "wake_words/"):
        assert forbidden not in firmware, f"assistant source in USB-only build: {forbidden}"
    mode_source = (ROOT / "main/boards/esp-sparkbot/otg_mode.cc").read_text(encoding="utf-8")
    assert "lv_" not in mode_source, "mode manager must not create a second UI"
    ui_source = (ROOT / "main/usb/usb_device.cc").read_text(encoding="utf-8")
    assert "lv_obj_clean(audio_panel_)" in ui_source
    assert "if (!audio_view_) return;" in ui_source
    # Theme-independent repaint contract: deleting widgets alone does not
    # guarantee that transparent RGB565 regions erase the old panel pixels.
    for obj in ("screen", "audio_panel_", "mic_pill_", "mic_dot_"):
        assert f"lv_obj_set_style_bg_opa({obj}, LV_OPA_COVER, 0);" in ui_source
    assert "lv_obj_invalidate(lv_obj_get_screen(audio_panel_));" in ui_source
    assert "lv_obj_set_style_text_font(body_, font_body_, 0);" in ui_source
    out = ROOT / ".tmp" / "usb_hot_switch_test"
    out.mkdir(parents=True, exist_ok=True)
    for name in ("esp_log.h", "lvgl.h", "nvs_flash.h", "usb_audio.h", "display.h",
                 "esp_timer.h", "audio_codec.h", "freertos/FreeRTOS.h", "freertos/task.h",
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
    if args.compile_only:
        print("USB host tests compiled; execution skipped")
        return
    if args.run_dir:
        args.run_dir.mkdir(parents=True, exist_ok=True)
        run_binary = args.run_dir.resolve() / binary.name
        if run_binary != binary:
            shutil.copyfile(binary, run_binary)
        binary = run_binary
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
