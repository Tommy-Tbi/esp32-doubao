#include "usb_device.h"
#include "usb_audio_bridge.h"
#include "codecs/es8311_audio_codec.h"
#include "boards/esp-sparkbot/config.h"
#include "boards/esp-sparkbot/otg_mode.h"
#include "boards/esp-sparkbot/sparkbot_keys.h"
#include "button.h"
#include "display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

LV_FONT_DECLARE(font_puhui_basic_20_4);

namespace {
class UsbDisplay : public Display {
public:
    void Initialize() {
        width_ = DISPLAY_WIDTH;
        height_ = DISPLAY_HEIGHT;
        spi_bus_config_t bus = {};
        bus.mosi_io_num = DISPLAY_MOSI_GPIO;
        bus.miso_io_num = GPIO_NUM_NC;
        bus.sclk_io_num = DISPLAY_CLK_GPIO;
        bus.quadwp_io_num = GPIO_NUM_NC;
        bus.quadhd_io_num = GPIO_NUM_NC;
        bus.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
        const esp_lcd_panel_io_spi_config_t io = {
            .cs_gpio_num = DISPLAY_CS_GPIO,
            .dc_gpio_num = DISPLAY_DC_GPIO,
            .spi_mode = 0,
            .pclk_hz = 40 * 1000 * 1000,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io, &panel_io_));
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = DISPLAY_RST_GPIO,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));
        const esp_err_t display_on = esp_lcd_panel_disp_on_off(panel_, true);
        if (display_on != ESP_ERR_NOT_SUPPORTED) ESP_ERROR_CHECK(display_on);
        ESP_ERROR_CHECK(gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(DISPLAY_BACKLIGHT_PIN, 1));

        lv_init();
        lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
#if CONFIG_SOC_CPU_CORES_NUM > 1
        port.task_affinity = 1;
#endif
        ESP_ERROR_CHECK(lvgl_port_init(&port));
        const lvgl_port_display_cfg_t config = {
            .io_handle = panel_io_,
            .panel_handle = panel_,
            .buffer_size = DISPLAY_WIDTH * 20,
            .double_buffer = false,
            .hres = DISPLAY_WIDTH,
            .vres = DISPLAY_HEIGHT,
            .monochrome = false,
            .rotation = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
            .color_format = LV_COLOR_FORMAT_RGB565,
            .flags = { .buff_dma = true, .swap_bytes = true },
        };
        auto* lcd = lvgl_port_add_disp(&config);
        ESP_ERROR_CHECK(lcd ? ESP_OK : ESP_FAIL);
        lv_display_set_offset(lcd, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
        DisplayLockGuard lock(this);
        auto* screen = lv_display_get_screen_active(lcd);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1320), 0);
        lv_obj_set_style_text_font(screen, &font_puhui_basic_20_4, 0);
        lv_obj_set_style_text_color(screen, lv_color_hex(0xE2E8F0), 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        auto label = [screen](lv_align_t align, int y) {
            auto* result = lv_label_create(screen);
            lv_obj_set_width(result, LV_PCT(90));
            lv_obj_set_style_text_font(result, &font_puhui_basic_20_4, 0);
            lv_label_set_long_mode(result, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(result, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_line_space(result, 8, 0);
            lv_obj_align(result, align, 0, y);
            return result;
        };
        status_ = label(LV_ALIGN_TOP_MID, 6);
        lv_obj_set_style_text_color(status_, lv_color_hex(0x38BDF8), 0);
        lv_label_set_text(status_, "SparkBot USB");
        body_ = label(LV_ALIGN_CENTER, 0);
        lv_label_set_text(body_, "音箱 + 麦克风\n连接电脑即可使用");
        footer_ = label(LV_ALIGN_BOTTOM_MID, -6);
        lv_obj_set_style_text_color(footer_, lv_color_hex(0x94A3B8), 0);
        lv_label_set_text(footer_, "BOOT 长按切换");
        notice_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto* self = static_cast<UsbDisplay*>(lv_timer_get_user_data(timer));
            lv_label_set_text(self->footer_, "BOOT 长按切换");
            lv_timer_pause(timer);
        }, 3500, this);
        lv_timer_pause(notice_timer_);
    }

    void SetStatus(const char* text) override {
        DisplayLockGuard lock(this);
        lv_label_set_text(status_, text && text[0] ? text : "SparkBot USB");
        lv_label_set_text(body_, text && std::strcmp(text, "USB 调试") == 0 ?
            "原生 Serial/JTAG\n长按 BOOT 5 秒\n返回 USB 音频" :
            "音箱 + 麦克风\n连接电脑即可使用");
    }

    void ShowNotification(const char* text, int duration_ms = 3000) override {
        DisplayLockGuard lock(this);
        lv_label_set_text(footer_, text);
        lv_timer_set_period(notice_timer_, std::max(duration_ms, 1));
        lv_timer_reset(notice_timer_);
        lv_timer_resume(notice_timer_);
    }

protected:
    bool Lock(int timeout_ms) override { return lvgl_port_lock(timeout_ms); }
    void Unlock() override { lvgl_port_unlock(); }

private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* footer_ = nullptr;
    lv_timer_t* notice_timer_ = nullptr;
};

class UsbDevice {
public:
    void Initialize() {
        const i2c_master_bus_config_t i2c = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = { .enable_internal_pullup = true },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c, &i2c_bus_));
        display_.Initialize();
        codec_ = new Es8311AudioCodec(i2c_bus_, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        ESP_ERROR_CHECK(bridge_.Initialize(codec_) ? ESP_OK : ESP_FAIL);
        ESP_ERROR_CHECK(bridge_.Start() ? ESP_OK : ESP_FAIL);
        OtgMode::GetInstance().Begin(&display_);
        InitializeKeys();
    }

private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    UsbDisplay display_;
    Es8311AudioCodec* codec_ = nullptr;
    UsbAudioBridge bridge_;
    ComboKeyDetector combo_;
    Button boot_{BOOT_BUTTON_GPIO, false, 5000, 0};
    TouchButton* touch_[TOUCH_BUTTON_NUM] = {};

    void ChangeVolume(int delta) {
        codec_->SetOutputVolume(std::clamp(codec_->output_volume() + delta, 0, 100));
        char text[32];
        snprintf(text, sizeof(text), "音量 %d%%", codec_->output_volume());
        display_.ShowNotification(text, 1500);
    }

    void InitializeKeys() {
        combo_.SetWindowMs(500);
        combo_.OnCombo([](uint32_t mask) { OtgMode::GetInstance().ToggleByKeys(mask); });
        boot_.OnPressDown([this]() { combo_.PressDown(COMBO_KEY_BOOT); });
        boot_.OnPressUp([this]() { combo_.PressUp(COMBO_KEY_BOOT); });
        boot_.OnLongPress([this]() {
            if (!combo_.IsComboActive()) OtgMode::GetInstance().ToggleByKeys();
        });
        const int pads[] = {TOUCH_BUTTON_PAD_1, TOUCH_BUTTON_PAD_2, TOUCH_BUTTON_PAD_3};
        const float sensitivity[] = {TOUCH_BUTTON_SENSITIVITY_1,
            TOUCH_BUTTON_SENSITIVITY_2, TOUCH_BUTTON_SENSITIVITY_3};
        for (int i = 0; i < TOUCH_BUTTON_NUM; ++i) {
            touch_[i] = new TouchButton(pads[i], sensitivity[i]);
            touch_[i]->OnPress([this, i]() { combo_.PressDown(i); });
            touch_[i]->OnRelease([this, i]() {
                // Decide before PressUp clears the chord latch on its last key.
                const bool chord = combo_.IsComboActive();
                combo_.PressUp(i);
                if (!chord && i == 1) ChangeVolume(-10);
                if (!chord && i == 2) ChangeVolume(10);
            });
        }
        TouchButton::StartAll();
    }
};
}  // namespace

void RunUsbAudioDevice() {
    static UsbDevice device;
    device.Initialize();
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
