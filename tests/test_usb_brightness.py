"""Host tests for brightness gestures, PWM configuration and NVS persistence.
Does not verify physical backlight brightness or shared-PA audio behavior.
"""
from pathlib import Path
import argparse
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FAKES = r'''
#pragma once
#include <cassert>
#include <cstdint>
using gpio_num_t = int;
using esp_err_t = int;
using nvs_handle_t = int;
constexpr int ESP_OK=0, LEDC_LOW_SPEED_MODE=0, LEDC_TIMER_10_BIT=10;
constexpr int LEDC_TIMER_0=0, LEDC_AUTO_CLK=0, LEDC_CHANNEL_0=0, LEDC_INTR_DISABLE=0;
constexpr int NVS_READONLY=0, NVS_READWRITE=1;
#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)
struct ledc_timer_config_t { int speed_mode,duty_resolution,timer_num,freq_hz,clk_cfg; };
struct ledc_channel_config_t { int gpio_num,speed_mode,channel,intr_type,timer_sel; uint32_t duty; int hpoint; };
inline int duty=0, commits=0, stored=75;
inline bool present=false, fail_commit=false;
inline int ledc_timer_config(const ledc_timer_config_t* t) {
    assert(t->freq_hz==25000 && t->duty_resolution==10); return 0;
}
inline int ledc_channel_config(const ledc_channel_config_t* c) {
    assert(c->gpio_num==46); duty=c->duty; return 0;
}
inline int ledc_set_duty(int,int,uint32_t value) { duty=value; return 0; }
inline int ledc_update_duty(int,int) { return 0; }
inline int nvs_open(const char*,int,nvs_handle_t* h) { *h=1; return 0; }
inline int nvs_get_i32(int,const char*,int32_t* value) {
    if (!present) return -1;
    *value=stored; return 0;
}
inline void nvs_close(int) {}
inline int nvs_set_i32(int,const char*,int32_t value) { stored=value; return 0; }
inline int nvs_commit(int) { ++commits; return fail_commit ? -1 : 0; }
'''
TEST = r'''
#include "usb/brightness_keys.h"
#include "usb/usb_backlight.h"
int main() {
    UsbBacklight bl;
    bl.Initialize(46);
    assert(bl.Saved()==75 && duty==1023);
    bl.Set(75); assert(duty==767);
    bl.Set(0); assert(duty==102);
    bl.Set(200); assert(duty==1023);
    assert(bl.Save(75) && commits==0);
    assert(bl.Save(35) && commits==1 && bl.Saved()==35);
    assert(bl.Save(35) && commits==1);
    fail_commit=true;
    assert(!bl.Save(45) && bl.Saved()==35);
    fail_commit=false;
    assert(bl.Save(45) && bl.Saved()==45);
    present=true; stored=0;
    UsbBacklight restored; restored.Initialize(46);
    assert(restored.Saved()==10);
    stored=120; restored.Initialize(46); assert(restored.Saved()==100);

    using K = BrightnessKeys;
    K k; k.Initialize(75);
    assert(k.Key(0,true,0)==K::None);
    assert(k.Poll(1999999)==K::None);
    assert(k.Poll(2000000)==K::Refresh && k.Editing());
    assert(k.Key(0,false,2000001)==K::None); // no unwanted mute/play
    assert(k.Key(2,true,3000000)==K::None);
    assert(k.Key(2,false,3000001)==K::Refresh && k.Value()==85);
    assert(k.Key(0,true,4000000)==K::None);
    assert(k.Key(0,false,4100000)==K::Closed && !k.Editing());
    k.Key(0,true,4200000);
    assert(k.Key(0,false,4300000)==K::Media);
    assert(k.Key(0,false,4300001)==K::None); // duplicate release

    // Long-press detection must also work when no periodic poll ran.
    K late;
    late.Key(0,true,0);
    assert(late.Key(0,false,2000000)==K::Refresh && late.Editing());
    assert(late.Poll(6999999)==K::None);
    assert(late.Poll(7000000)==K::Closed);
    // No timeout while keys are held, no accidental long-press retrigger.
    K held; held.Initialize(100);
    held.Key(0,true,0); held.Poll(2000000);
    assert(held.Poll(9000000)==K::None && held.Editing());
    held.Key(0,false,9000001);
    held.Key(2,true,10000000);
    assert(held.Poll(20000000)==K::None);
    assert(held.Key(2,false,20000001)==K::Refresh && held.Value()==100);
    assert(held.Poll(25000001)==K::Closed);
    // Overlapping top/side keys cannot enter settings or issue media.
    K chord;
    chord.Key(1,true,0); chord.Key(0,true,100);
    assert(chord.Poll(3000000)==K::None && !chord.Editing());
    assert(chord.Key(0,false,3000001)==K::None);
    chord.Key(1,false,3000002);
    // Both volume pads in settings are brightness inputs, not USB-mode keys.
    K pair; pair.Initialize(10);
    pair.Key(0,true,0); pair.Key(0,false,2000000);
    pair.Key(1,true,2100000); pair.Key(2,true,2100010);
    assert(pair.Poll(6000000)==K::None && pair.Editing());
    pair.Key(1,false,6000001); assert(pair.Value()==10);
    pair.Key(2,false,6000002); assert(pair.Value()==20);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=shutil.which('clang++') or shutil.which('g++'))
    p.add_argument('--run-dir', type=Path)
    args = p.parse_args()
    if not args.cxx:
        p.error('C++20 compiler required')
    out = ROOT / '.tmp' / 'usb_brightness_test'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'fakes.h').write_text(FAKES, encoding='utf-8')
    for name in ('driver/ledc.h', 'nvs.h', 'esp_err.h'):
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "fakes.h"\n', encoding='utf-8')
    source = out / 'test.cc'
    source.write_text(TEST, encoding='utf-8')
    binary = out / 'usb_brightness_test.exe'
    subprocess.run([args.cxx, '-std=c++20', '-UNDEBUG', '-I'+str(out),
                    '-I'+str(ROOT/'main'), str(source), '-o', str(binary)], check=True)
    if args.run_dir:
        args.run_dir.mkdir(parents=True, exist_ok=True)
        target = args.run_dir.resolve() / binary.name
        if target != binary:
            shutil.copyfile(binary, target)
        binary = target
    subprocess.run([str(binary)], check=True)
    print('USB brightness tests passed (gestures, PWM, persistence)')


if __name__ == '__main__':
    main()
