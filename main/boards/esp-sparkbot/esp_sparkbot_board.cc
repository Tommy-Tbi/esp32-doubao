#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "settings.h"
#include "sparkbot_keys.h"
#include "otg_mode.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>

#include "esp32_camera.h"

#define TAG "esp_sparkbot"

class SparkBotEs8311AudioCodec : public Es8311AudioCodec {
private:

public:
    SparkBotEs8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
                        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                        gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk = true)
        : Es8311AudioCodec(i2c_master_handle, i2c_port, input_sample_rate, output_sample_rate,
                             mclk,  bclk,  ws,  dout,  din,pa_pin,  es8311_addr,  use_mclk) {}

    void EnableOutput(bool enable) override {
        if (enable == output_enabled_) {
            return;
        }
        if (enable) {
            Es8311AudioCodec::EnableOutput(enable);
        } else {
           // Nothing todo because the display io and PA io conflict
        }
    }
};

class EspSparkBot : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    TouchButton* touch_buttons_[TOUCH_BUTTON_NUM] = {nullptr, nullptr, nullptr};
    ComboKeyDetector combo_keys_;
    esp_timer_handle_t combo_timer_ = nullptr;
    Display* display_;
    Esp32Camera* camera_;
    light_mode_t light_mode_ = LIGHT_MODE_ALWAYS_ON;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_GPIO;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_GPIO;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // 开始/停止对话（boot 按键与 PLAY 按键共用）
    void ToggleChat() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();

        if (state == kDeviceStateStarting) {
            // 本项目不做配网：开机/连接中不做任何处理
            ESP_LOGI(TAG, "button: ignored while starting");
            return;
        }

        if (state == kDeviceStateIdle) {
            ESP_LOGI(TAG, "button: starting chat");
            app.StartListening();
        } else {
            ESP_LOGI(TAG, "button: stopping chat");
            app.StopListening();
        }
    }

    void ChangeVolume(int delta) {
        auto* codec = GetAudioCodec();
        int volume = std::clamp(codec->output_volume() + delta, 0, 100);
        codec->SetOutputVolume(volume);

        char text[32];
        snprintf(text, sizeof(text), "音量 %d%%", volume);
        if (display_) {
            display_->ShowNotification(text, 1500);
        }
    }

    // 面板按键引脚：TOUCH_PAD_NUM1 / TOUCH_PAD_NUM2（touch_element，内部电容通道）
    // 详见 config.h 中对本机实际走线的说明
    void InitializeKeys() {
        (void)0;  // touch_buttons_ 在成员初始化列表中已创建
    }

    void InitializeButtons() {
        // ------------------------------------------------------------------
        // 组合键：USB 音频与原生 USB Serial/JTAG 双向热切换
        // ------------------------------------------------------------------
        const esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) { static_cast<EspSparkBot*>(arg)->combo_keys_.Poll(); },
            .arg = this,
            .name = "combo_hold",
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &combo_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(combo_timer_, 20000));
        combo_keys_.OnCombo([this](uint32_t mask) {
            ESP_LOGI(TAG, "combo %s pressed", ComboKeyDetector::Describe(mask).c_str());
            OtgMode::GetInstance().ToggleByKeys(mask);
        });

        // Boot button: toggle chat state (connect/disconnect)
        boot_button_.OnPressDown([this]() {
            combo_keys_.PressDown(COMBO_KEY_BOOT);
        });
        boot_button_.OnPressUp([this]() {
            combo_keys_.PressUp(COMBO_KEY_BOOT);
        });
        boot_button_.OnClick([this]() {
            if (combo_keys_.IsComboActive()) {
                return;  // 组合键期间不执行单键动作
            }
            ToggleChat();
        });
        // BOOT 长按 5 秒：同样请求双向热切换，不重启，不阻塞按键任务
        boot_button_.OnLongPress([this]() {
            if (combo_keys_.IsComboActive()) {
                return;
            }
            OtgMode::GetInstance().ToggleByKeys();
        });

        // ------------------------------------------------------------------
        // 触摸键（本设备 2 个）：触摸键 1 对话，触摸键 2 音量减
        // 左右音量触摸键（PAD2/PAD3）同时持续按住 3 秒 => 切换 USB 模式
        // ------------------------------------------------------------------
        // Touch button 1: 开始/停止对话
        touch_buttons_[0]->OnPress([this]() {
            combo_keys_.PressDown(COMBO_KEY_1);
            if (combo_keys_.IsComboActive()) {
                return;
            }
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            ESP_LOGI(TAG, "Touch button 1 (pad 1) pressed, state=%d", state);

            char text[48];
            switch (state) {
                case kDeviceStateIdle:
                    snprintf(text, sizeof(text), "开始聆听");
                    app.StartListening();
                    break;
                case kDeviceStateListening:
                    snprintf(text, sizeof(text), "已停止收音");
                    app.StopListening();
                    break;
                case kDeviceStateSpeaking:
                    snprintf(text, sizeof(text), "已打断播报");
                    app.AbortSpeaking(kAbortReasonNone);
                    break;
                default:
                    snprintf(text, sizeof(text), "请稍候再试");
                    break;
            }
            if (display_) {
                display_->ShowNotification(text, 1500);
            }
        });
        touch_buttons_[0]->OnRelease([this]() {
            combo_keys_.PressUp(COMBO_KEY_1);
        });

        // 同时在串口打印 PAD1 状态（设备没有串口，PDR1 在屏幕上看到响应）
        ESP_LOGI(TAG, "Touch button 1 (pad 1) handler registered");
        ESP_LOGI(TAG, "Touch button 2 (pad 2) handler registered");
        ESP_LOGI(TAG, "Touch button 3 (pad 3) handler registered");

        // Touch button 2: 音量 -10
        touch_buttons_[1]->OnPress([this]() {
            combo_keys_.PressDown(COMBO_KEY_2);
            if (combo_keys_.IsComboActive()) {
                return;
            }
            ESP_LOGI(TAG, "Touch button 2 (pad 2) pressed - Volume Down");
            ChangeVolume(-10);
        });
        touch_buttons_[1]->OnRelease([this]() {
            combo_keys_.PressUp(COMBO_KEY_2);
        });

        // Touch button 3: 音量 +10
        touch_buttons_[2]->OnPress([this]() {
            combo_keys_.PressDown(COMBO_KEY_3);
            if (combo_keys_.IsComboActive()) {
                return;
            }
            ESP_LOGI(TAG, "Touch button 3 (pad 3) pressed - Volume Up");
            ChangeVolume(10);
        });
        touch_buttons_[2]->OnRelease([this]() {
            combo_keys_.PressUp(COMBO_KEY_3);
        });
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_GPIO;
        io_config.dc_gpio_num = DISPLAY_DC_GPIO;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeCamera() {
#if CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
        // DVP pin configuration
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = SPARKBOT_CAMERA_D0,
                [1] = SPARKBOT_CAMERA_D1,
                [2] = SPARKBOT_CAMERA_D2,
                [3] = SPARKBOT_CAMERA_D3,
                [4] = SPARKBOT_CAMERA_D4,
                [5] = SPARKBOT_CAMERA_D5,
                [6] = SPARKBOT_CAMERA_D6,
                [7] = SPARKBOT_CAMERA_D7,
            },
            .vsync_io = SPARKBOT_CAMERA_VSYNC,
            .de_io = SPARKBOT_CAMERA_HSYNC,
            .pclk_io = SPARKBOT_CAMERA_PCLK,
            .xclk_io = SPARKBOT_CAMERA_XCLK,
        };

        // 复用 I2C 总线
        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,  // 不初始化新的 SCCB，使用现有的 I2C 总线
            .i2c_handle = i2c_bus_,  // 使用现有的 I2C 总线句柄
            .freq = 100000,  // 100kHz
        };

        // DVP configuration
        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = SPARKBOT_CAMERA_RESET,
            .pwdn_pin = SPARKBOT_CAMERA_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = SPARKBOT_CAMERA_XCLK_FREQ,
        };
#endif

        // Main video configuration
        esp_video_init_config_t video_config = {
#if CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
            .dvp = &dvp_config,
#endif
        };
        
        camera_ = new Esp32Camera(video_config);

        Settings settings("sparkbot", false);
        // 考虑到部分复刻使用了不可动摄像头的设计，默认启用翻转
        bool camera_flipped = static_cast<bool>(settings.GetInt("camera-flipped", 1));
        camera_->SetHMirror(camera_flipped);
        camera_->SetVFlip(camera_flipped);
    }

    /*
        ESP-SparkBot 的底座
        https://gitee.com/esp-friends/esp_sparkbot/tree/master/example/tank/c2_tracked_chassis
    */
    void InitializeEchoUart() {
        uart_config_t uart_config = {
            .baud_rate = ECHO_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 0,
            .source_clk = UART_SCLK_DEFAULT,
            .flags = 0,
        };
        int intr_alloc_flags = 0;

        ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
        ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD, UART_ECHO_RTS, UART_ECHO_CTS));

        SendUartMessage("w2");
    }

    void SendUartMessage(const char * command_str) {
        uint8_t len = strlen(command_str);
        uart_write_bytes(ECHO_UART_PORT_NUM, command_str, len);
        ESP_LOGI(TAG, "Sent command: %s", command_str);
    }

    void InitializeTools() {
        
    }

public:
    EspSparkBot() : boot_button_(BOOT_BUTTON_GPIO, false, 5000, 0) {
        touch_buttons_[0] = new TouchButton(TOUCH_BUTTON_PAD_1, TOUCH_BUTTON_SENSITIVITY_1);
        touch_buttons_[1] = new TouchButton(TOUCH_BUTTON_PAD_2, TOUCH_BUTTON_SENSITIVITY_2);
        touch_buttons_[2] = new TouchButton(TOUCH_BUTTON_PAD_3, TOUCH_BUTTON_SENSITIVITY_3);
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        // 先确定按键引脚与极性，再注册回调
        InitializeKeys();
        InitializeButtons();
        InitializeCamera();
        InitializeEchoUart();
        InitializeTools();
        // 所有 TouchButton 已创建、所有外设已初始化完成，再启动 touch element
        // （IDF 要求 touch_element_start() 晚于 element 创建）
        TouchButton::StartAll();
        // TouchButton::ReBaseline();  // 临时关掉，看触摸键 1 是否恢复
        // USB mode worker starts in Application after AudioService is ready.
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
         static SparkBotEs8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(EspSparkBot);
