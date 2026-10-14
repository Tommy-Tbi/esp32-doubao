#!/usr/bin/env python
"""把编译产物收集到一个目录（默认 build/flash/），并生成合并固件 + 烧写脚本。

用法（在任意工作目录均可）：
    python scripts/package_firmware.py [-b build] [-o build/flash] [--no-merge]

产物：
    <out>/00_bootloader.bin
    <out>/01_partition-table.bin
    <out>/02_ota_data_initial.bin
    <out>/03_hello_world.bin      (app, 只烧 OTA 时用)
    <out>/04_generated_assets.bin  (assets 分区)
    <out>/firmware.bin             (完整合并镜像，烧到 0x0)
    <out>/flash.sh / flash.bat     (一键烧写完整固件)
    <out>/flash_args.txt           (偏移列表，便于人工核对)
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import textwrap

# Windows 控制台中文输出
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def load_flash_files(build_dir):
    """从 flasher_args.json 读取需要烧写的文件列表 [(offset, src_path)]。"""
    args_file = os.path.join(build_dir, "flasher_args.json")
    if not os.path.isfile(args_file):
        raise SystemExit(f"[package] 找不到 {args_file}，请先完整构建一次")
    with open(args_file, "r", encoding="utf-8") as f:
        data = json.load(f)

    flash_files = []
    write_args = data.get("write_flash_args", [])
    for offset, path in data.get("flash_files", {}).items():
        src = path if os.path.isabs(path) else os.path.join(build_dir, path)
        if not os.path.isfile(src):
            print(f"[package] 跳过不存在的文件: {src}")
            continue
        flash_files.append((int(offset, 16), src))
    flash_files.sort(key=lambda x: x[0])
    return flash_files, write_args


def collect(build_dir, out_dir):
    flash_files, _ = load_flash_files(build_dir)
    os.makedirs(out_dir, exist_ok=True)

    copied = []
    for idx, (offset, src) in enumerate(flash_files):
        name = os.path.basename(src)
        dst = os.path.join(out_dir, f"{idx:02d}_{name}")
        if os.path.abspath(src) != os.path.abspath(dst):
            shutil.copy2(src, dst)
        copied.append((offset, dst, name))
        print(f"[package] {offset:#010x}  {name}  ({os.path.getsize(dst)} bytes)")
    return copied


def write_manifest(out_dir, copied):
    lines = []
    for offset, path, name in copied:
        lines.append(f"{offset:#010x}  {name}")
    with open(os.path.join(out_dir, "flash_args.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def merge(out_dir, copied, chip, flash_mode, flash_size, flash_freq):
    """用 esptool merge_bin 生成单个可烧到 0x0 的完整固件。"""
    merged = os.path.join(out_dir, "firmware.bin")
    cmd = [sys.executable, "-m", "esptool", "--chip", chip,
           "merge_bin", "-o", merged,
           "--flash_mode", flash_mode,
           "--flash_freq", flash_freq,
           "--flash_size", flash_size]
    for offset, path, name in copied:
        cmd += [f"{offset:#x}", path]
    try:
        subprocess.run(cmd, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"[package] 合并固件失败（可忽略，仍可用 flash.sh 逐个烧）: {e}")
        return None
    print(f"[package] 完整固件: {merged} ({os.path.getsize(merged)} bytes)")
    return merged


def write_flash_scripts(out_dir, copied, write_args, port=None):
    port = port or "COM3"
    args = " ".join(write_args) if write_args else ""

    sh_pairs = " ".join(f'"{offset:#x}" "$DIR/{name}"' for offset, _, name in copied)
    sh = textwrap.dedent(f"""\
        #!/usr/bin/env bash
        # 一键烧录完整固件（先 cd 到本脚本所在目录）
        set -e
        DIR="$(cd "$(dirname "${{BASH_SOURCE[0]}}")" && pwd)"
        PORT="${{PORT:-{port}}}"
        esptool.py {args} write_flash {sh_pairs} "$PORT"
        """)
    sh_path = os.path.join(out_dir, "flash.sh")
    with open(sh_path, "w", encoding="utf-8") as f:
        f.write(sh)
    try:
        os.chmod(sh_path, 0o755)
    except OSError:
        pass

    bat_pairs = " ".join(f'"{offset:#x}" "%~dp0{name}"' for offset, _, name in copied)
    bat = (
        "@echo off\r\n"
        "REM 一键烧录完整固件\r\n"
        f"set PORT=%PORT_SERIAL%\r\n"
        "if \"%PORT%\"==\"\" set PORT=" + port + "\r\n"
        f"esptool.py {args} write_flash {bat_pairs} %PORT%\r\n"
    )
    with open(os.path.join(out_dir, "flash.bat"), "w", encoding="utf-8", newline="") as f:
        f.write(bat)


def main():
    parser = argparse.ArgumentParser(description="收集编译产物并生成完整固件")
    parser.add_argument("-b", "--build", default="build", help="构建目录，默认 build")
    parser.add_argument("-o", "--out", default=None, help="输出目录，默认 <build>/flash")
    parser.add_argument("-p", "--port", default=None, help="默认串口，写入烧写脚本")
    parser.add_argument("--no-merge", action="store_true", help="不生成合并固件 firmware.bin")
    args = parser.parse_args()

    build_dir = os.path.abspath(args.build)
    out_dir = os.path.abspath(args.out or os.path.join(build_dir, "flash"))

    copied = collect(build_dir, out_dir)
    write_manifest(out_dir, copied)

    # 读取 chip / flash 参数
    chip = "esp32s3"
    settings = {}
    args_file = os.path.join(build_dir, "flasher_args.json")
    with open(args_file, "r", encoding="utf-8") as f:
        settings = json.load(f)
    chip = settings.get("extra_esptool_args", {}).get("chip", chip)
    fs = settings.get("flash_settings", {})
    if not args.no_merge:
        merge(out_dir, copied, chip,
              fs.get("flash_mode", "dio"),
              fs.get("flash_size", "16MB"),
              fs.get("flash_freq", "80m"))

    _, write_args = load_flash_files(build_dir)
    write_flash_scripts(out_dir, copied, write_args, args.port)
    print(f"[package] 全部产物已收集到: {out_dir}")


if __name__ == "__main__":
    main()