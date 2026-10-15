#include "usb_device.h"
#include "usb_screen.h"
#include <esp_heap_caps.h>
#include "usb_audio_bridge.h"
#include "usb_audio.h"
#include "codecs/es8311_audio_codec.h"
#include "boards/esp-sparkbot/config.h"
#include "boards/esp-sparkbot/otg_mode.h"
#include "boards/esp-sparkbot/sparkbot_keys.h"
#include "button.h"
#include "display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <esp_err.h>
#include <esp_timer.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_14_1);
constexpr const char* kKeyHint = "左右键同按 3 秒切换";

namespace {
constexpr const char* kVolumeFormat = "%d%%";
constexpr const char* kMicInUse = "Mic in use";
constexpr const char* kMicIdle = "Mic idle";
// Shared palette keeps the small RGB565 screen calm and legible.
constexpr uint32_t kBackground = 0x090F19;
constexpr uint32_t kSurface = 0x131F2E;
constexpr uint32_t kBorder = 0x253548;
constexpr uint32_t kAccent = 0x62D9E8;
constexpr uint32_t kText = 0xF1F5FA;
constexpr uint32_t kMuted = 0x8A9BB0;
constexpr uint32_t kSuccess = 0x65DDB0;
constexpr uint32_t kWarning = 0xFB8798;

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
        // Do not depend on a theme to supply an opaque background. In partial
        // RGB565 rendering, every dirty region must repaint its old pixels.
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(screen, lv_color_hex(kBackground), 0);
        lv_obj_set_style_text_font(screen, &font_puhui_20_4, 0);
        lv_obj_set_style_text_color(screen, lv_color_hex(kText), 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        auto label = [screen](lv_align_t align, int y) {
            auto* result = lv_label_create(screen);
            lv_obj_set_width(result, LV_PCT(90));
            lv_obj_set_style_text_font(result, &font_puhui_20_4, 0);
            lv_label_set_long_mode(result, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(result, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_line_space(result, 2, 0);
            lv_obj_align(result, align, 0, y);
            return result;
        };
        auto* brand = lv_label_create(screen);
        lv_obj_set_style_text_font(brand, &font_puhui_14_1, 0);
        lv_obj_set_style_text_color(brand, lv_color_hex(kMuted), 0);
        lv_label_set_text(brand, "SPARKBOT");
        lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 16, 14);
        status_ = label(LV_ALIGN_TOP_RIGHT, 12);
        lv_obj_set_width(status_, 126);
        lv_obj_set_style_text_font(status_, &font_puhui_14_1, 0);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(status_, LV_ALIGN_TOP_RIGHT, -16, 14);
        lv_obj_set_style_text_color(status_, lv_color_hex(kAccent), 0);
        lv_label_set_text(status_, "USB 音频");
        InitializeContentPanel(screen);
        InitializeAudioPanel();
        footer_ = label(LV_ALIGN_BOTTOM_MID, -8);
        lv_obj_set_style_text_color(footer_, lv_color_hex(kMuted), 0);
        lv_obj_set_style_text_font(footer_, &font_puhui_14_1, 0);
        lv_label_set_text(footer_, kKeyHint);
        notice_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto* self = static_cast<UsbDisplay*>(lv_timer_get_user_data(timer));
            lv_label_set_text(self->footer_, kKeyHint);
            lv_timer_pause(timer);
        }, 3500, this);
        lv_timer_pause(notice_timer_);
        screen_pixels_ = static_cast<uint8_t*>(heap_caps_calloc(
            240 * 240, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        ESP_ERROR_CHECK(screen_pixels_ ? ESP_OK : ESP_ERR_NO_MEM);
        screen_canvas_ = lv_canvas_create(screen);
        lv_canvas_set_buffer(screen_canvas_, screen_pixels_, 240, 240, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(screen_canvas_, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_flag(screen_canvas_, LV_OBJ_FLAG_HIDDEN);
        ESP_ERROR_CHECK(UsbScreen::Initialize() ? ESP_OK : ESP_ERR_NO_MEM);
    }

    void UpdateUsbScreen() {
        DisplayLockGuard lock(this);
        const bool connected = UsbAudio::GetInstance().IsConnected();
        if (audio_view_ && connected && UsbScreen::TakeFrame(screen_pixels_)) {
            lv_obj_remove_flag(screen_canvas_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(screen_canvas_);
        }
        if (!audio_view_ || !connected) {
            lv_obj_add_flag(screen_canvas_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void SetStatus(const char* text) override {
        DisplayLockGuard lock(this);
        lv_label_set_text(status_, text && text[0] ? text : "USB 音频");
        const bool audio = !text || !text[0] || std::strcmp(text, "USB 音频") == 0;
        if (audio != audio_view_) {
            // One content tree: destroy the previous view, never cover it.
            lv_obj_clean(audio_panel_);
            body_ = volume_label_ = volume_bar_ = mute_slash_ = nullptr;
            mic_label_ = mic_dot_ = mic_pill_ = nullptr;
            for (auto& wave : waves_) wave = nullptr;
            displayed_volume_ = displayed_mic_ = -1;
            audio_view_ = audio;
            if (audio) {
                InitializeAudioPanel();
            } else {
                body_ = lv_label_create(audio_panel_);
                lv_obj_set_width(body_, 188);
                lv_obj_set_style_text_font(body_, &font_puhui_20_4, 0);
                lv_obj_set_style_text_color(body_, lv_color_hex(kText), 0);
                lv_obj_set_style_text_align(body_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_text_line_space(body_, 8, 0);
                lv_label_set_long_mode(body_, LV_LABEL_LONG_WRAP);
                lv_obj_center(body_);
            }
        }
        if (!audio) {
            lv_obj_add_flag(screen_canvas_, LV_OBJ_FLAG_HIDDEN);
            const bool debug = std::strcmp(text, "USB 调试") == 0;
            const bool error = std::strcmp(text, "USB 异常") == 0;
            lv_label_set_text(body_, debug ?
                "原生 Serial/JTAG\n左右键同按 3 秒\n返回 USB 音频" : error ?
                "USB 初始化失败\n已停止切换以防冲突\n按 RESET 复位设备" :
                "正在交接 USB 接口\n电脑将自动重新识别\n无需拔线或重启");
            lv_obj_set_style_text_color(status_,
                lv_color_hex(error ? kWarning : kAccent), 0);
        } else {
            lv_obj_set_style_text_color(status_, lv_color_hex(kAccent), 0);
        }
        // A mode transition also clears pixels outside the content card (old
        // header/footer glyphs and rounded corners). No continuous full redraw.
        lv_obj_invalidate(lv_obj_get_screen(audio_panel_));
    }

    void ShowNotification(const char* text, int duration_ms = 3000) override {
        DisplayLockGuard lock(this);
        lv_label_set_text(footer_, text ? text : "");
        lv_timer_set_period(notice_timer_, std::max(duration_ms, 1));
        lv_timer_reset(notice_timer_);
        lv_timer_resume(notice_timer_);
    }

    void UpdateAudioState(int volume, bool mic_in_use) {
        volume = std::clamp(volume, 0, 100);
        DisplayLockGuard lock(this);
        if (!audio_view_) return;
        if (volume != displayed_volume_) {
            displayed_volume_ = volume;
            char text[16];
            snprintf(text, sizeof(text), kVolumeFormat, volume);
            lv_label_set_text(volume_label_, text);
            lv_bar_set_value(volume_bar_, volume, LV_ANIM_OFF);
            lv_obj_set_style_text_color(volume_label_,
                lv_color_hex(volume == 0 ? kMuted : kText), 0);
            const int waves = volume == 0 ? 0 : volume <= 33 ? 1 : volume <= 66 ? 2 : 3;
            for (int i = 0; i < 3; ++i) {
                if (i < waves) lv_obj_remove_flag(waves_[i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(waves_[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (volume == 0) lv_obj_remove_flag(mute_slash_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(mute_slash_, LV_OBJ_FLAG_HIDDEN);
        }
        if (displayed_mic_ != static_cast<int>(mic_in_use)) {
            displayed_mic_ = mic_in_use;
            lv_label_set_text(mic_label_, mic_in_use ? kMicInUse : kMicIdle);
            lv_obj_set_style_text_color(mic_label_,
                lv_color_hex(mic_in_use ? kSuccess : kMuted), 0);
            lv_obj_set_style_bg_color(mic_dot_,
                lv_color_hex(mic_in_use ? kSuccess : 0x475569), 0);
            lv_obj_set_style_bg_color(mic_pill_,
                lv_color_hex(mic_in_use ? 0x173B35 : 0x1C2A3B), 0);
        }
    }

protected:
    bool Lock(int timeout_ms) override { return lvgl_port_lock(timeout_ms); }
    void Unlock() override { lvgl_port_unlock(); }

private:
    uint8_t* screen_pixels_ = nullptr;
    lv_obj_t* screen_canvas_ = nullptr;
    void InitializeContentPanel(lv_obj_t* screen) {
        audio_panel_ = lv_obj_create(screen);
        lv_obj_remove_style_all(audio_panel_);
        lv_obj_set_style_bg_opa(audio_panel_, LV_OPA_COVER, 0);
        lv_obj_set_size(audio_panel_, 216, 166);
        // Fixed 240x240 layout: header, audio card, then a separate notice area.
        lv_obj_align(audio_panel_, LV_ALIGN_TOP_MID, 0, 42);
        lv_obj_remove_flag(audio_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(audio_panel_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_all(audio_panel_, 0, 0);
        lv_obj_set_style_bg_color(audio_panel_, lv_color_hex(kSurface), 0);
        lv_obj_set_style_border_width(audio_panel_, 1, 0);
        lv_obj_set_style_border_color(audio_panel_, lv_color_hex(kBorder), 0);
        lv_obj_set_style_radius(audio_panel_, 20, 0);
    }

    void InitializeAudioPanel() {
        auto* icon = lv_obj_create(audio_panel_);
        lv_obj_set_size(icon, 76, 60);
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 12, 25);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        // Vector icon avoids relying on symbol glyphs in the bundled CJK font.
        auto line = [icon](const lv_point_precise_t* points, uint32_t count, uint32_t color) {
            auto* obj = lv_line_create(icon);
            lv_line_set_points(obj, points, count);
            lv_obj_set_style_line_width(obj, 3, 0);
            lv_obj_set_style_line_rounded(obj, true, 0);
            lv_obj_set_style_line_color(obj, lv_color_hex(color), 0);
            return obj;
        };
        static const lv_point_precise_t speaker[] = {
            {8, 22}, {20, 22}, {34, 10}, {34, 50}, {20, 38}, {8, 38}, {8, 22}
        };
        static const lv_point_precise_t wave1[] = {{42, 22}, {46, 30}, {42, 38}};
        static const lv_point_precise_t wave2[] = {{50, 16}, {57, 30}, {50, 44}};
        static const lv_point_precise_t wave3[] = {{60, 10}, {69, 30}, {60, 50}};
        static const lv_point_precise_t slash[] = {{6, 8}, {70, 52}};
        line(speaker, 7, kAccent);
        waves_[0] = line(wave1, 3, kAccent);
        waves_[1] = line(wave2, 3, kAccent);
        waves_[2] = line(wave3, 3, kAccent);
        mute_slash_ = line(slash, 2, kWarning);
        lv_obj_add_flag(mute_slash_, LV_OBJ_FLAG_HIDDEN);

        auto* caption = lv_label_create(audio_panel_);
        lv_obj_set_style_text_font(caption, &font_puhui_14_1, 0);
        lv_obj_set_style_text_color(caption, lv_color_hex(kMuted), 0);
        lv_label_set_text(caption, "VOLUME");
        lv_obj_align(caption, LV_ALIGN_TOP_RIGHT, -16, 18);
        volume_label_ = lv_label_create(audio_panel_);
        lv_obj_set_style_text_font(volume_label_, &lv_font_montserrat_36, 0);
        lv_label_set_text(volume_label_, "--%");
        lv_obj_align(volume_label_, LV_ALIGN_TOP_RIGHT, -14, 40);
        volume_bar_ = lv_bar_create(audio_panel_);
        lv_obj_set_size(volume_bar_, 180, 6);
        lv_obj_align(volume_bar_, LV_ALIGN_TOP_MID, 0, 98);
        lv_bar_set_range(volume_bar_, 0, 100);
        lv_obj_set_style_bg_opa(volume_bar_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(volume_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(volume_bar_, lv_color_hex(kBorder), LV_PART_MAIN);
        lv_obj_set_style_bg_color(volume_bar_, lv_color_hex(kAccent), LV_PART_INDICATOR);
        lv_obj_set_style_radius(volume_bar_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_radius(volume_bar_, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

        mic_pill_ = lv_obj_create(audio_panel_);
        lv_obj_remove_style_all(mic_pill_);
        lv_obj_set_style_bg_opa(mic_pill_, LV_OPA_COVER, 0);
        lv_obj_set_size(mic_pill_, 180, 30);
        lv_obj_align(mic_pill_, LV_ALIGN_TOP_MID, 0, 120);
        lv_obj_remove_flag(mic_pill_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(mic_pill_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_all(mic_pill_, 0, 0);
        lv_obj_set_style_border_width(mic_pill_, 0, 0);
        lv_obj_set_style_radius(mic_pill_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(mic_pill_, lv_color_hex(0x1C2A3B), 0);
        mic_label_ = lv_label_create(mic_pill_);
        lv_obj_set_style_text_font(mic_label_, &font_puhui_14_1, 0);
        lv_obj_set_style_text_color(mic_label_, lv_color_hex(kMuted), 0);
        lv_label_set_text(mic_label_, kMicIdle);
        lv_obj_align(mic_label_, LV_ALIGN_CENTER, 8, 0);
        mic_dot_ = lv_obj_create(mic_pill_);
        lv_obj_remove_style_all(mic_dot_);
        lv_obj_set_style_bg_opa(mic_dot_, LV_OPA_COVER, 0);
        lv_obj_set_size(mic_dot_, 8, 8);
        lv_obj_set_style_radius(mic_dot_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(mic_dot_, 0, 0);
        lv_obj_set_style_pad_all(mic_dot_, 0, 0);
        lv_obj_remove_flag(mic_dot_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(mic_dot_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(mic_dot_, lv_color_hex(0x475569), 0);
        lv_obj_align(mic_dot_, LV_ALIGN_LEFT_MID, 18, 0);
    }

    bool audio_view_ = true;  // Protected by the LVGL display lock.
    lv_obj_t* audio_panel_ = nullptr;
    lv_obj_t* volume_label_ = nullptr;
    lv_obj_t* volume_bar_ = nullptr;
    lv_obj_t* waves_[3] = {};
    lv_obj_t* mute_slash_ = nullptr;
    lv_obj_t* mic_pill_ = nullptr;
    lv_obj_t* mic_label_ = nullptr;
    lv_obj_t* mic_dot_ = nullptr;
    int displayed_volume_ = -1;
    int displayed_mic_ = -1;
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
        display_.UpdateAudioState(codec_->output_volume(), false);
        OtgMode::GetInstance().Begin(&display_);
        InitializeKeys();
    }

public:
    void PollKeys() {
        combo_.Poll();
        display_.UpdateUsbScreen();
        const int64_t now = esp_timer_get_time();
        if (now - last_ui_update_us_ < 100000) return;
        last_ui_update_us_ = now;
        int volume;
        {
            std::lock_guard<std::mutex> lock(volume_mutex_);
            volume = codec_->output_volume();
        }
        display_.UpdateAudioState(volume, UsbAudio::GetInstance().IsMicrophoneActive());
    }

private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    UsbDisplay display_;
    Es8311AudioCodec* codec_ = nullptr;
    UsbAudioBridge bridge_;
    ComboKeyDetector combo_;
    Button boot_{BOOT_BUTTON_GPIO, false, 5000, 0};
    TouchButton* touch_[TOUCH_BUTTON_NUM] = {};
    int64_t last_media_touch_us_ = -350000;
    int64_t last_ui_update_us_ = -100000;
    std::mutex volume_mutex_;

    void PlayPause() {
        const int64_t now = esp_timer_get_time();
        if (now - last_media_touch_us_ < 350000) return;
        last_media_touch_us_ = now;
        if (OtgMode::GetInstance().IsBlocked()) return;
        const bool queued = UsbAudio::GetInstance().RequestPlayPause();
        display_.ShowNotification(queued ? "Play/Pause requested" : "Media control unavailable", 1500);
    }

    void ChangeVolume(int delta) {
        int volume;
        {
            std::lock_guard<std::mutex> lock(volume_mutex_);
            codec_->SetOutputVolume(std::clamp(codec_->output_volume() + delta, 0, 100));
            volume = codec_->output_volume();
        }
        char text[32];
        snprintf(text, sizeof(text), "音量 %d%%", volume);
        display_.ShowNotification(text, 1500);
    }

    void InitializeKeys() {
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
                if (!chord && i == 0) PlayPause();
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
    for (;;) {
        device.PollKeys();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
