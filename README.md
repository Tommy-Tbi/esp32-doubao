# ESP-SPARKBOT USB 音箱 + 麦克风

当前分支是独立的 ESP32-S3 USB 音频固件，不运行语音助手、唤醒词、AFE/VAD、云端对话或网络配网。

## 功能

- USB UAC2 音箱：电脑播放 → ES8311 → 本机喇叭。
- USB UAC2 麦克风：本机麦克风 → ES8311 → 电脑录音。
- 双向均为 **48kHz / 16bit / 单声道**，codec 使用同样的采样率，不经过 Opus 或重采样。
- USB 音频与**芯片原生 USB Serial/JTAG**双向热切换，不需要拔线或重启；电脑会断开旧设备并重新枚举，新模式不能与旧模式同时使用。
- 保存音量和最后成功选择的 USB 模式。首次启动默认音频模式。
- 内置 240×240 状态界面，无需下载表情、字体或语音模型资源。蓝色表示音频连接，黄色表示切换，红色表示异常。

### 录音通路

```text
麦克风 → ES8311 ADC / I2S 48kHz → 唯一采集任务 → USB 麦克风缓冲 → UAC2 IN
电脑音频 → UAC2 OUT → 播放任务 → I2S 48kHz / ES8311 DAC → 喇叭
```

采集不依赖设备待机、唤醒、聆听或网络状态。电脑打开麦克风接口后直接上传原始 PCM；关闭录音后停止入队并清理缓存。I2S 始终排空，不会在下次录音时发送很久以前的 DMA 数据。切换 USB 时的数据访问受到互斥保护。

原始录音没有 VAD 门控、自动降噪或回声消除；同时播放时，麦克风可能拾取本机喇叭声音，这是正常声学串音。

## 按键

| 操作 | 功能 |
| --- | --- |
| 左右音量触摸键（PAD2 + PAD3）同时持续按住 5 秒 | 音频 ↔ 原生 Serial/JTAG |
| BOOT 长按 5 秒 | 同上 |
| BOOT 短按 | 无对话操作 |
| 触摸键 1 | 不参与模式切换；无单独对话操作 |
| 触摸键 2 单独松开 | 音量 -10 |
| 触摸键 3 单独松开（有该通道的板型） | 音量 +10 |

不足 5 秒松开任一键即取消计时；左右两键全部松开后才能再次触发组合键；切换期间忽略重复请求。单键音量在松开时处理，避免组成组合键时误改音量。触摸通道、灵敏度和引脚见 `main/boards/esp-sparkbot/config.h`。

## 构建

使用 **ESP-IDF ≥5.4** 和 ESP32-S3 工具链：

```sh
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p <PORT> flash
```

依赖已精简到 codec、LVGL/字体、按键和 TinyUSB。更改后的 `main/idf_component.yml` 会在重新配置时触发组件管理器更新 `dependencies.lock`；不要手动编辑托管组件。

- SparkBot 选择独立的 `main/usb/firmware.cmake` 构建入口，不编译旧的 `application.cc`、`audio_service.cc`、协议、AFE、唤醒词、摄像头或底盘控制代码。旧多板型源码保留在仓库，不属于当前固件。
- `USB_AUDIO_DEVICE` 自动选择 `TINYUSB_NO_DEFAULT_TASK`。应用管理 USB 任务并等待其协作退出，避免在持有 TinyUSB 内部锁时强行删除任务。
- 确认 `USB_AUDIO_SPEAKER=y`、`USB_AUDIO_MIC=y`。已有本地 `sdkconfig` 不会被 `sdkconfig.defaults` 全部覆盖，应在 menuconfig 中检查。
- 沿用原有 16MB 分区布局以保持 NVS 地址；旧 assets/model 分区不再参与此固件运行，也不需要刷新。
- 编译后由 `scripts/package_firmware.py` 输出 `build/flash/` 烧录包。

### 日志接口与硬件注意事项

音频模式独占原生 USB PHY，USB Serial/JTAG 串口会消失。默认使用独立 **UART1：TX=GPIO38、RX=GPIO48、115200 baud** 输出日志，需外接 USB-UART。UART0 GPIO43/44 已用于 LCD，不应作为当前板子的日志引脚。

现有 `sdkconfig` 请检查控制台配置：

```text
CONFIG_ESP_CONSOLE_UART_CUSTOM=y
CONFIG_ESP_CONSOLE_UART_NUM=1
CONFIG_ESP_CONSOLE_UART_TX_GPIO=38
CONFIG_ESP_CONSOLE_UART_RX_GPIO=48
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
```

GPIO46 在本板同时关联 PA/背光。独立音频服务保持 codec 双工接口开启，不在切换中关闭 PA 或启用冲突的背光 PWM。摄像头和底盘 UART 均不初始化。

## 录音检查

1. 在电脑输入设备中选择 **ESP32-S3 USB Speaker+Mic** 对应的麦克风，允许录音应用访问麦克风。
2. 打开录音应用并选择该输入，对麦克风讲话；不需要按键开始对话。
3. UART 应出现 `USB microphone stream started`。每 5 秒的 `UsbBridge` 日志包含：
   - `host_recording=1`：主机已开启麦克风接口。
   - `blocks`：成功处理的采集块数（每块 10ms，不是 USB 包数）。
   - `failures`：codec 读取或 USB 入队失败次数。
   - `peak`：原始 PCM 峰值；讲话时应有明显变化。
4. `host_recording=0` 时先检查电脑选中的输入设备/权限。持续 `peak=0` 时检查 ADC、麦克风接线和 codec 日志；出现 `Microphone read failed` 时采集任务不会再把旧数据当成有效音频发送。
5. 关闭录音应看到 `stream stopped`；重新打开后应立刻恢复，无需重启。

## 测试与板端验收

主机模拟测试（需要 C++20 编译器）：

```sh
python tests/test_usb_hot_switch.py
```

共享盘禁止执行时可用 `--run-dir <本机目录>`。测试覆盖原始麦克风分流、录音开关、读取失败、热切换时的数据丢弃、播放补静音、20 次模式往返、失败分支、组合键和 UI 状态。**模拟接口不代表 USB 枚举、I2S 或实机录音已通过验收。**

板端请验证：

1. 音频模式出现 VID/PID `303A:4011` 的音箱和麦克风，独立播放、独立录音、同时播放录音均正常。
2. 保持数据线不动，切换到原生 Serial/JTAG，验证串口/OpenOCD；这不是旧实现的 `303A:4012` 模拟 CDC。
3. 关闭调试会话，重新按组合键返回音频，再次播放和录音。
4. 重复至少 20 次并持续录音数分钟，确认无持续丢块、内存泄漏、看门狗复位或声音中断。
5. 分别在两种模式下复位，确认恢复已保存的模式，并能再次切回音频。

### 恢复烧录

固件正常时可先切到原生 Serial/JTAG 再使用烧录工具。若固件未运行或 USB 切换异常：按住 **BOOT(GPIO0)**，按 **RESET/EN**（或断电上电），再松开 BOOT，选择 ROM 下载接口烧录。

此方式要求 eFuse 未禁用相应 USB/ROM 下载功能。使用连接 ESP32-S3 原生 USB GPIO19/20 的数据接口。
