"""抓 ESP32-S3 USB-Serial-JTAG 控制台日志。

复位会让 USB 重新枚举，pyserial 旧句柄会失效，
所以复位后要循环关闭/重开端口才能读到启动日志。
用法: python scripts/usb_console.py [COM口] [抓取秒数]
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0


def open_port(baud=115200):
    s = serial.Serial()
    s.port = PORT
    s.baudrate = baud
    s.timeout = 0.2
    s.open()
    return s


def pulse_reset():
    """复位 ESP32-S3（USB-Serial-JTAG：DTR=GPIO0 需拉高不选中下载模式，RTS=EN）"""
    s = open_port()
    time.sleep(0.2)
    s.dtr = True   # 释放 GPIO0（不进入 download 模式）
    s.rts = True   # EN 拉低 -> 复位
    time.sleep(0.12)
    s.rts = False  # 释放 EN -> 正常启动
    time.sleep(0.05)
    s.close()


def main():
    pulse_reset()

    deadline = time.time() + DURATION
    buf = b""
    last_open = 0.0
    while time.time() < deadline:
        try:
            s = open_port()
        except Exception as e:
            time.sleep(0.3)
            continue
        last_open = time.time()
        try:
            s.dtr = True
            s.rts = False
            s.reset_input_buffer()
            while time.time() < deadline and time.time() - last_open < 6:
                chunk = s.read(4096)
                if chunk:
                    buf += chunk
                    sys.stdout.write(chunk.decode("utf-8", "replace"))
                    sys.stdout.flush()
        except Exception:
            pass
        finally:
            try:
                s.close()
            except Exception:
                pass
        if time.time() - deadline > -1 and time.time() - last_open >= 6:
            break

    print(f"\n[captured {len(buf)} bytes]")


if __name__ == "__main__":
    main()
