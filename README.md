| Supported Targets | ESP32-S3 |
| ----------------- | -------- |

# 豆包语音助手 (esp32-doubao)

基于 ESP32-S3 的豆包（字节跳动火山引擎）实时语音对话设备。
支持语音唤醒、ASR、TTS 实时流式对话，对话文本与表情显示在 LCD 屏幕上。

## 硬件

- **主控**：ESP32-S3（带 8MB PSRAM）
- **板型**：esp-sparkbot
- **屏幕**：SPI LCD + LVGL 图形界面
- **音频**：ES8311 codec
- **WiFi**：802.11 b/g/n

## 功能特性

- 实时语音对话（豆包大模型）
- 流式文本显示（CHAT_RESPONSE event 550 实时刷新）
- 流式语音合成（OGG/Opus 解码播放）
- 多轮对话上下文保持
- LCD 表情 / 文字界面
- WiFi 配网（AP 模式 + Web 配网）

## 项目结构

```
esp32-doubao/
├── main/                          主程序
│   ├── application.cc/h           应用层、状态机
│   ├── protocols/
│   │   └── douyin_voice_protocol.cc  豆包 WebSocket 协议实现
│   ├── audio/
│   │   └── audio_service.cc       Opus 编解码、队列管理
│   ├── display/                   LVGL 显示
│   └── ...
├── partitions/v2/16m.csv          16MB Flash 分区表
├── scripts/
│   ├── idf.sh                     ESP-IDF 编译封装
│   ├── idf_run.py                 idf.py 包装
│   └── env.sh                     环境变量
├── managed_components/            第三方组件
├── build/                         编译输出
└── sdkconfig                      项目配置
```

## 编译与烧录

### 编译

```bash
./scripts/idf.sh build
```

### 烧录

```bash
# 自动检测串口
./scripts/idf.sh flash

# 指定串口
./scripts/idf.sh -p COM3 flash

# 手动烧录（完整命令）
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0        build/bootloader/bootloader.bin \
  0x8000     build/partition_table/partition-table.bin \
  0xd000     build/ota_data_initial.bin \
  0x20000    build/hello_world.bin \
  0x800000   build/generated_assets.bin
```

### 查看日志

```bash
./scripts/idf.sh -p COM3 monitor
# 退出: Ctrl + ]
```

## 首次使用

1. 开机不再自动进入配网：有已保存 WiFi 时自动连接，否则离线待机；连接超时也不自动开启配网热点。
2. 待机时长按 **触摸键 2（GPIO2）5 秒**，手动进入 WiFi 配网模式。
3. 手机连接屏幕显示的 `Xiaozhi-XXXX` 热点，浏览器打开 `http://192.168.4.1`。
4. 在网页中配置家庭 WiFi 凭据，完成后设备尝试连接。
5. 开机及默认待机图标改为通用扬声器；USB 音频不依赖 WiFi 配网。

## 豆包 API 配置

**仓库中不保存任何密钥**（`sdkconfig` 已加入 `.gitignore`，`Kconfig` 默认值为空）。
克隆后请在本地配置，二选一：

**方式一：menuconfig（写入本地 sdkconfig，不会上传）**

```bash
idf.py menuconfig
# Xiaozhi Assistant → Douyin/App AppID          填 App ID
# Xiaozhi Assistant → Douyin/Access Key (Token) 填 Access Key
idf.py build
```

**方式二：运行时写入 NVS（固件里也不带密钥）**

在 `douyin` 命名空间下写入 `app_id` / `access_key`，
`DouyinVoiceProtocol::OpenAudioChannel()` 会优先读取 NVS，为空时才回退到
`CONFIG_DOUYIN_APP_ID` / `CONFIG_DOUYIN_ACCESS_KEY`。

> `sdkconfig.defaults` 只保存非机密项（板型、分区、Flash 大小、语言、
> 配网方式、豆包开关），不含任何凭据。

API 端点：`wss://openspeech.bytedance.com/api/v3/realtime/dialogue`
Resource ID：`volc.speech.dialog`
App Key：`PlgvMymc7f3tQnJ6`

## 关键事件协议

| Event ID | 名称             | 说明                       |
| -------- | ---------------- | -------------------------- |
| 1        | StartConnection  | 客户端发起连接             |
| 100      | StartSession     | 启动对话会话               |
| 200      | TaskRequest      | 发送音频                   |
| 350      | TTS_SENTENCE_START | 一句 TTS 开始             |
| 351      | TTS_SENTENCE_END | 一句 TTS 音频结束          |
| 359      | TTS_ENDED        | 全部 TTS 结束              |
| 450      | ASR_INFO         | 用户开始说话（可用于打断） |
| 451      | ASR_RESPONSE     | ASR 识别结果               |
| 459      | ASR_ENDED        | ASR 结束                   |
| 550      | CHAT_RESPONSE    | 流式回复文本（可多次）     |
| 559      | CHAT_ENDED       | LLM 回复结束               |

## 状态机

```
unknown → starting → wifi_configuring → idle
                                    ↓
                          (user input) → listening → speaking → idle/listening
```

设备状态定义在 `application.h`：
- `kDeviceStateIdle`
- `kDeviceStateConnecting`
- `kDeviceStateListening`
- `kDeviceStateSpeaking`

## USB 音箱 + 麦克风（USB 复合音频设备）

设备可作为一个 **USB 复合音频设备** 直接插到 PC 上，无需安装驱动：

| 方向 | 说明 |
| ---- | ---- |
| PC → 设备 | PC 枚举出「USB 扬声器」，PC 播放的音频由设备本地喇叭播出（带反馈通道，48kHz/16bit/单声道） |
| 设备 → PC | PC 枚举出「USB 麦克风」，设备麦克风音频上传给 PC（48kHz/16bit/单声道） |

### 开启方式

```bash
idf.py menuconfig
# USB_AUDIO_DEVICE_CONFIG → USB Audio Device (Speaker + Microphone) = y
#   Enable USB Speaker (PC -> device speaker)          = y
#   Enable USB Microphone (device mic -> PC)           = y
idf.py build && idf.py flash monitor
```

也可以在 `sdkconfig` 中直接写：

```
CONFIG_USB_AUDIO_DEVICE=y
CONFIG_USB_AUDIO_SPEAKER=y
CONFIG_USB_AUDIO_MIC=y
```

### 说明

- 基于 TinyUSB 的 UAC2 设备类（`espressif/esp_tinyusb`，会在构建时自动拉取）。
- 打开后 USB-OTG 独占内部 PHY，**同一原生 USB 口的 USB-Serial-JTAG 串口会消失**；调试需使用独立 UART 接口，并将控制台配置为 UART。
- 采样率固定 48kHz、16bit、单声道；端点最大包长 98 字节（96 字节音频 + 一个样本余量）。音量由本地控制，不声明未实现的 USB 音量/静音控制。
- Speaker/Mic 开关控制数据通路，USB 始终枚举两个接口；关闭 Mic 时输出静音。
- 默认配置未开启 USB 音频；需要显式设置 `CONFIG_USB_AUDIO_DEVICE=y`。
- 设备本地正在播 TTS 时，USB 音箱任务会主动让路，避免两路声音叠加。

### 从 OTG 音频模式恢复烧录

ESP-SPARKBOT 的 BOOT 键连接 GPIO0。通常不需要旧固件配合：

1. 按住 **BOOT**。
2. 按一下 **RESET/EN**；没有复位键时，保持 BOOT 按下并断电重新上电。
3. 松开 BOOT，等待电脑重新识别 ROM 下载接口，重新选择实际出现的 COM 端口烧录。
4. 烧录完成后正常复位，应用启用 USB 音频时该 COM 口会再次消失。

使用连接 ESP32-S3 原生 USB（GPIO19=D-、GPIO20=D+）的数据口，不是仅供电接口。
上述方式以未通过 eFuse 禁用 USB/ROM 下载且启动绑带满足芯片要求为前提；
若板子另有 USB-UART 桥，也可经 UART0 配合 BOOT/RESET 烧录。

当前代码没有“软件切回烧录”的命令或按键处理；`UsbAudio::Stop()` 只暂停音频，
不会释放 PHY，也不会进入 ROM 下载。普通重启会再次运行应用、重新启用 OTG，
因此不能替代上述 BOOT + 硬件复位操作。

## 已修复问题

### 1. 豆包长回复时状态卡死

**症状**：当豆包回复内容很长时，设备说完后无法回到 listening 状态。

**原因**：`AudioService::on_playback_finished` 回调在播放队列清空时立即触发，但此时 `TTS_SENTENCE_END` (351) 或 `TTS_ENDED` (359) 事件尚未被 WebSocket 任务处理完，导致 `tts_ended_` 仍为 `false`，状态转换失败后无重试机制。

**修复**（`main/application.cc`）：在 TTS stop 事件处理中，如果发现播放队列已经为空，直接执行状态转换：

```cpp
// TTS 停止时，如果播放队列已空，立即转换状态
if (audio_service_.IsPlaybackQueueEmpty() && GetDeviceState() == kDeviceStateSpeaking) {
    if (listening_mode_ == kListeningModeManualStop) {
        SetDeviceState(kDeviceStateIdle);
    } else {
        SetDeviceState(kDeviceStateListening);
    }
}
```

### 2. 文本流式刷新延迟

**症状**：豆包每说一段，屏幕上的文字才一次性更新，造成语音与文字不同步。

**原因**：`chat_text` 消息通过 `Schedule()` 异步队列处理，导致 `display->SetChatMessage()` 调用时机不确定。

**修复**（`main/application.cc`）：去掉 `Schedule()`，直接同步调用：

```cpp
} else if (strcmp(type->valuestring, "chat_text") == 0) {
    auto text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text) && strlen(text->valuestring) > 0) {
        display->SetChatMessage("assistant", text->valuestring);
    }
}
```

## 分区表（16MB Flash）

| Name    | Type | SubType | Offset   | Size     | 说明           |
| ------- | ---- | ------- | -------- | -------- | -------------- |
| nvs     | data | nvs     | 0x9000   | 0x4000   | 配置存储       |
| otadata | data | ota     | 0xd000   | 0x2000   | OTA 状态       |
| phy_init| data | phy     | 0xf000   | 0x1000   | RF 校准        |
| ota_0   | app  | ota_0   | 0x20000  | 0x3f0000 | 应用程序 (~4MB)|
| ota_1   | app  | ota_1   | -        | 0x3f0000 | OTA 备份       |
| assets  | data | spiffs  | 0x800000 | 8MB      | 资源（字库等） |

## 常见问题

### 烧录后启动失败 / 屏幕不亮

确认烧录时**包含 `generated_assets.bin` 到 0x800000**。字库等资源在 SPIFFS 分区，未烧录会导致 LVGL 找不到字体而崩溃。

### 启动后无串口日志

- 检查 USB 线是否支持数据传输（非仅充电线）
- 确认串口驱动已安装（CH340 / CP210x）
- 串口监视器需要选 115200 8N1

### 状态卡在 speaking

参考上文"已修复问题"第 1 条。如修改后仍有问题，可用 `./scripts/idf.sh -p COM3 monitor` 抓取 `TTS stop` 与 `Playback finished callback` 的时序日志。

## 开发工具链

- ESP-IDF v5.5.3
- Python 3.x（在 ESP-IDF venv 中）
- CMake / Ninja
- 目标芯片：ESP32-S3
