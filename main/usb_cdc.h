#pragma once

#include <stdbool.h>

// CDC (Communication Device Class) only TinyUSB device — used as the "JTAG mode"
// for ESP-SparkBot. PC sees a single CDC ACM serial port (COM port) for console
// logs and debugging. No audio functionality in this mode.
//
// 运行时切换：调用 UsbAudioUninstall() 卸载 UAC2 描述符，然后 UsbCdcInstall()
// 安装本描述符。PC 端会重新枚举，先看到 UAC2 消失，然后看到 CDC COM 出现。
// 反向切换（CDC -> UAC2）需要重启 + NVS 标志（由 BOOT 长按触发）。

bool UsbCdcInstall();   // 安装 CDC-only TinyUSB 设备
void UsbCdcUninstall(); // 卸载（释放 USB 控制器）
bool UsbCdcIsInstalled();

// 把一行字符串写到 CDC 端口（用于切换后的简易日志输出；非阻塞）
void UsbCdcPrint(const char* text);
