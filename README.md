# ESP-SPARKBOT USB 屏幕 + 音箱 + 麦克风

当前分支是独立的 ESP32-S3 USB 屏幕/音频复合设备固件，不运行语音助手、唤醒词、AFE/VAD、云端对话或网络配网。

## USB 扩展屏幕（XFZ1986 v2）

参考 `win10_idd_xfz1986_usb_graphic_driver_display` 的 v2 帧协议和 RGB565 编码器实现，不是 USB UVC 视频设备。

- 复合设备 ID 改为 **303A:2986**；屏幕占接口 **MI_00**，音箱/麦克风占接口 1–4，媒体键占接口 5。音频和屏幕可同时使用；原生 Serial/JTAG 仍独占 USB PHY。
- 屏幕接口名称为 `ESP32S3 Display`，产品字符串为 `ESP32S3 Display_R240x240_Ergb16_Fps5_Bl113`（参数后缀供驱动解析，不可删除）：240×240、RGB565、小端像素、目标上限 5 FPS。**尚未实现 JPEG**，实际帧率和同时录放音稳定性需要实机验证。
- 接收完整画面或边界内矩形，校验尺寸/长度，处理 Windows 的 `0xff` 空帧、短包和断连复位。不支持的编码丢弃，不向驱动宣告 JPEG 能力。
- 首帧到达后用 LVGL canvas 显示电脑桌面，并隐藏整个本机 UI 容器，避免底层音量图标/文字继续参与绘制。视图切换时全屏重绘，避免 USB 接收任务直接操作 LCD。静止桌面保留最后一帧；拔线或切到调试模式恢复原有状态界面。桌面显示时音量提示被遮挡，但音量键和媒体键仍有效。
- 接收、待显示、canvas 共约 **338 KiB PSRAM**，默认配置启用 SparkBot 的 Octal PSRAM；其他内存型号请先修改配置。分配失败会报错，不以空指针继续运行。

### 电脑端使用

1. 在 ESP-IDF 环境重新配置并编译，建议使用新的构建目录。旧 sdkconfig 必须启用 `TINYUSB_VENDOR_ENABLED=y`、`TINYUSB_VENDOR_COUNT=1`，并设置 `TINYUSB_VENDOR_EPSIZE=64`、`TINYUSB_VENDOR_RX_BUFSIZE=64`、`TINYUSB_VENDOR_TX_BUFSIZE=64`，同时启用 PSRAM。
2. Windows 安装参考仓库 `windows_driver_sign/xfz1986_usb_graphic_250224_rc_sign.exe` 的新版驱动；应匹配复合设备的 `USB\VID_303A&PID_2986&MI_00`。不要将音频接口或整个复合设备替换为 WinUSB。旧源码 INF 仅匹配整个设备，不能直接当作本固件的复合设备安装包。
3. 在系统显示设置选择扩展桌面；音箱和麦克风仍用系统 UAC2 驱动。Linux 使用参考仓库新版 DRM 驱动（此组合尚未实测）。
4. 验收红/绿/蓝纯色、文字方向、静止桌面、动态桌面与双向音频并行、拔插，以及至少 20 次 Serial/JTAG 往返。5 FPS 是协议声明，不是已测性能。

协议主机测试：`python tests/test_usb_screen.py`；共享盘禁止执行时添加 `--run-dir <本机目录>`。测试覆盖碎片化、边界、错误长度、超大帧、空帧和复位；不替代固件编译、驱动绑定和板端验收。

## 功能

- USB UAC2 音箱：电脑播放 → ES8311 → 本机喇叭。
- USB UAC2 麦克风：本机麦克风 → ES8311 → 电脑录音。
- 顶部触摸键（PAD1）：麦克风未使用时发送 HID 播放/暂停；麦克风正在使用时改为本机麦克风静音/取消静音，不发送媒体键。
- 双向均为 **48kHz / 16bit / 单声道**，codec 使用同样的采样率，不经过 Opus 或重采样。
- USB 音频与**芯片原生 USB Serial/JTAG**双向热切换，不需要拔线或重启；电脑会断开旧设备并重新枚举，新模式不能与旧模式同时使用。
- 保存音量和最后成功选择的 USB 模式。首次启动默认音频模式。
- 内置 240×240 状态界面，无需下载表情、字体或语音模型资源。音频页使用上下两张等宽卡片：青色 `Speaker`、绿色 `Microphone`，各有小型线条图标、实时能量条和独立状态文字。不再显示音量百分比、大号喇叭或旧的嵌套麦克风胶囊；亮度设置页仍保留亮度百分比。
- 全部设备界面文字使用英语，内嵌 OFL 授权的 Abel TrueType 矢量字体，由 LVGL TinyTTF 在 16/20/38px 实时栅格化，不依赖外部文件系统。字体和授权位于 `main/usb/fonts/`，来源为 Google Fonts 的 `ofl/abel`。喇叭声波改为真正的圆弧，不再使用尖角折线。
- 两条能量条使用 PCM 峰值计算（-60～0 dBFS 映射到条长，快速上升、约 300ms 衰减，UI 约 50–60ms 更新），不显示数值，也不是环境分贝/SPL 测量。Speaker 测量 codec 音量调节前的电脑播放信号，本机音量为零时条归零并显示 `Output muted`；其余时候显示 `USB playback`。
- Microphone 始终显示**本地输入能量**：未开启 USB 录音、USB 断连、USB 麦克风静音或上传缓冲拥堵时，都继续反映实际采集信号。采样失败立即清零，真实安静时自然衰减；复用唯一 I2S 采集任务，不新增读取任务，也不主动开启电脑录音。
- 麦克风状态文字分别为 `Local input / USB idle`、`Local input / USB live`、`Local input / USB muted`，与能量条独立。静音只让上传电脑的 PCM 为零，本地能量条仍会跳动；关闭录音或断连会清除静音状态。这不是会议软件的全局静音按钮。
- 麦克风使用期间（包括本机静音）拒绝所有按键的 USB 模式切换，提示 `Mic in use: USB mode locked`；需要先让电脑关闭录音接口。模式切换与主机打开录音接口互斥，避免检查后立即开始录音的竞争。

### 录音通路

```text
麦克风 → ES8311 ADC / I2S 48kHz → 唯一采集任务 → USB 麦克风缓冲 → UAC2 IN
电脑音频 → UAC2 OUT → 播放任务 → I2S 48kHz / ES8311 DAC → 喇叭
```

采集不依赖设备待机、唤醒、聆听或网络状态。电脑打开麦克风接口后，未静音时上传原始 PCM，静音时上传零 PCM；关闭录音后停止入队并清理缓存。I2S 始终排空，不会在下次录音时发送很久以前的 DMA 数据。切换 USB 时的数据访问受到互斥保护。

原始录音没有 VAD 门控、自动降噪或回声消除；同时播放时，麦克风可能拾取本机喇叭声音，这是正常声学串音。

## 屏幕亮度

- 使用与 `main/boards/common/backlight.cc` 相同的 **25kHz、10-bit LEDC PWM** 调节真实背光，不使用黑色遮罩。百分比直接对应 PWM 占空比，不再叠加旧代码的 `BRIGHTNESS_SCALE=25`。
- 范围 **10–100%**，每次加减 10 个百分点，首次默认 **75%**。退出设置时才写入 NVS `display/usb_brightness`，值未改变时不重复写入；与旧 `display/brightness` 分开，避免混淆旧的缩放语义。
- **顶部长按 2 秒**进入亮度设置；左右键调节；顶部短按或再次长按退出，也可松开全部按键后等待 **5 秒**自动保存退出。长按后的释放不会误触发媒体键或麦克风静音。
- 设置页使用全屏、不透明背景，不再让旧音量图标或桌面从卡片边缘露出；退出时短暂显示 `Brightness saved` 或 `Save failed`。设置期间左右键不修改音量，也不触发 USB 模式切换；BOOT 长按也暂时屏蔽。麦克风正在使用时仍允许调整亮度。
- GPIO46 同时连接 LCD_BLK 和功放 PA_CTRL。此固件将该引脚统一交给 PWM 管理，codec 的 PA GPIO 参数设为 `GPIO_NUM_NC`，避免 codec 再次配置 GPIO46、覆盖 PWM。启动时先保持最大 PWM 占空比，再应用保存的占空比。**这没有解除硬件共线；低亮度下功放是否正常启动、是否产生杂音，需要实机验证。**

## 按键

| 操作 | 功能 |
| --- | --- |
| 左右音量触摸键（PAD2 + PAD3）同时持续按住 3 秒 | 音频 ↔ 原生 Serial/JTAG |
| BOOT 长按 5 秒 | 同上 |
| BOOT 短按 | 无对话操作 |
| 顶部触摸键 1（PAD1）短按 | 普通界面：麦克风使用时切换静音，否则播放/暂停；亮度设置中：保存退出；调试模式普通短按无操作 |
| 顶部触摸键 1（PAD1）长按 2 秒 | 进入/退出亮度设置，不触发短按功能 |
| 触摸键 2 单独松开 | 音量 -10 |
| 触摸键 3 单独松开（有该通道的板型） | 音量 +10 |

播放/暂停使用标准 USB HID Consumer Control（0x00CD），由电脑系统/播放器处理；设备无法获知播放器真实状态，不显示推测的“正在播放/已暂停”。每次触摸松开发送按下和释放，350ms 内重复触摸被忽略；USB 未连接时不缓存操作。PAD1 与顶部物理键的对应关系需板端确认，不能使用与 codec I2C 冲突的 PAD4。

不足 3 秒松开任一键即取消计时；左右两键全部松开后才能再次触发组合键；切换期间忽略重复请求。单键音量在松开时处理，避免组成组合键时误改音量。触摸通道、灵敏度和引脚见 `main/boards/esp-sparkbot/config.h`。

## 构建

使用 **ESP-IDF ≥5.4**、ESP32-S3 工具链、CMake 和 Ninja。在已加载 ESP-IDF 环境的终端中运行，工作目录为项目根目录；不能单独配置 `main/usb/`。

### 直接使用 CMake

```sh
# 使用独立构建目录和 sdkconfig，避免沿用旧的 HID_COUNT=0 等配置。
# $PWD 为项目根目录，以下写法适用于 Bash / PowerShell。
cmake -S . -B build-cmake -G Ninja -DIDF_TARGET=esp32s3 -DSDKCONFIG="$PWD/build-cmake/sdkconfig" -DSDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults"

# 编译
cmake --build build-cmake

# 修改 Kconfig 配置（修改后重新编译）
cmake --build build-cmake --target menuconfig
```

源码/CMake 文件修改后可重新运行配置命令，再编译。`sdkconfig.defaults` 只提供默认值；已有 `build-cmake/sdkconfig` 不会被默认值全部覆盖。原生编译产物位于 `build-cmake/`，不再自动合并固件或生成烧录包。

烧录可使用 ESP-IDF 工具，指定同一构建目录：

```sh
idf.py -B build-cmake -p <PORT> flash
```

以上直接 CMake 流程尚未完成实际配置、固件编译和板端验收。

### 使用 idf.py（备选）

```sh
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p <PORT> flash
```

依赖已精简到 codec、LVGL/字体、按键和 TinyUSB。更改后的 `main/idf_component.yml` 会在重新配置时触发组件管理器更新 `dependencies.lock`；不要手动编辑托管组件。

- SparkBot 选择独立的 `main/usb/firmware.cmake` 构建入口，不编译旧的 `application.cc`、`audio_service.cc`、协议、AFE、唤醒词、摄像头或底盘控制代码。旧多板型源码保留在仓库，不属于当前固件。
- `USB_AUDIO_DEVICE` 自动选择 `TINYUSB_NO_DEFAULT_TASK`。应用管理 USB 任务并等待其协作退出，避免在持有 TinyUSB 内部锁时强行删除任务。
- 确认 `USB_AUDIO_SPEAKER=y`、`USB_AUDIO_MIC=y`、`TINYUSB_HID_ENABLED=y`、`TINYUSB_HID_COUNT=1`。已有本地 `sdkconfig` 不会被 `sdkconfig.defaults` 全部覆盖，应在 menuconfig 中检查。
- 沿用原有 16MB 分区布局以保持 NVS 地址；旧 assets/model 分区不再参与此固件运行，也不需要刷新。
- 编译仅生成 ESP-IDF 原生应用、bootloader 和分区表等产物，不自动合并 `firmware.bin` 或生成 `build/flash/` 烧录包。可使用 `idf.py -p <PORT> flash` 烧录。

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

GPIO46 在本板同时关联 PA/背光，现由背光 PWM 模块独占控制，codec 不再操作此 GPIO。独立音频服务保持 codec 双工接口开启，USB 切换不停止背光 PWM。摄像头和底盘 UART 均不初始化。

## 录音检查

1. 在电脑输入设备中选择 **ESP32S3 MIC**（USB 复合设备产品名为 **ESP32S3 Display_R240x240_Ergb16_Fps5_Bl113**）对应的麦克风，允许录音应用访问麦克风。
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
python tests/test_usb_media_control.py
python tests/test_usb_brightness.py
```

共享盘禁止执行时可用 `--run-dir <本机目录>`。测试覆盖原始麦克风分流、录音开关、读取失败、热切换时的数据丢弃、播放补静音、20 次模式往返、失败分支、组合键和 UI 状态。媒体控制测试覆盖按下/释放顺序、忙端点及发送失败重试、重复请求、断连后丢弃待发操作。**模拟接口不代表 USB 枚举、I2S 或实机录音已通过验收。**

板端请验证：

1. 音频模式出现 VID/PID `303A:2986` 的音箱和麦克风，独立播放、独立录音、同时播放录音均正常。
2. 保持数据线不动，切换到原生 Serial/JTAG，验证串口/OpenOCD；这不是旧实现的 `303A:4012` 模拟 CDC。
3. 关闭调试会话，重新按组合键返回音频，再次播放和录音。
4. 重复至少 20 次并持续录音数分钟，确认无持续丢块、内存泄漏、看门狗复位或声音中断。
5. 分别在两种模式下复位，确认恢复已保存的模式，并能再次切回音频。
6. 播放电脑音乐，单次触摸 PAD1 验证暂停/恢复；长时间按住不应重复切换，释放只触发一次。确认左右音量键、3 秒组合键不受影响，开始录音后 PAD1 应只切换麦克风静音、不控制播放器；检查录音波形变为静音再恢复，USB 录音流不断开，静音期间也无法切换 USB 模式。调试模式或 USB 未连接时触摸不应在返回音频后补发。
7. 音频页应仅显示 Speaker/Microphone 两张能量卡片，无旧百分比、大号音量图标或重叠控件。播放不同强度的音频检查 Speaker 条；音量降为零显示 `Output muted` 并清空条。电脑未录音时对麦克风讲话，Microphone 条也应变化；开启录音、静音、关闭录音、断连 USB 时，本地输入条继续变化而状态文字正确切换。静音时电脑录到的仍必须是零 PCM。真实安静时能量自然回落，切换调试/异常/亮度页不能残留旧卡片。
8. 音频功能分别命名为 **ESP32S3 Speaker** 和 **ESP32S3 MIC**；修改名称后需重新枚举 USB（设备版本已更新为 0x0105）。若系统沿用旧描述符，可在设备管理器移除旧设备后重连；播放器必须支持系统媒体键。

亮度功能另需板端验收：

1. 在本机音频界面、USB 桌面和 Serial/JTAG 模式分别长按顶部键，检查全屏设置页显示、左右调节、顶部退出和超时保存。
2. 依次测试 10%、25%（默认 75% 连减五次）、50%、75%、100% 的显示效果；持续播放音乐和录音，检查低亮度是否出现断音、噪声或功放启动失败。
3. 设置后重启，确认恢复保存值；不触摸时不得反复写入 NVS。
4. 设置中左右键同按超过 3 秒，不应切换 USB；退出后重新按组合键才允许切换。长按顶部键不能附带播放/暂停或麦克风静音。
5. 录音期间调光，不应停止 USB 录音；离开设置后录音仍按原有规则禁止 USB 模式切换。

### 恢复烧录

固件正常时可先切到原生 Serial/JTAG 再使用烧录工具。若固件未运行或 USB 切换异常：按住 **BOOT(GPIO0)**，按 **RESET/EN**（或断电上电），再松开 BOOT，选择 ROM 下载接口烧录。

此方式要求 eFuse 未禁用相应 USB/ROM 下载功能。使用连接 ESP32-S3 原生 USB GPIO19/20 的数据接口。
