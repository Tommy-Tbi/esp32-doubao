#include "usb_device.h"
#include "brightness_keys.h"
#include "speaker_ui.h"
#include "usb_backlight.h"
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
#include <freertos/semphr.h>

#include "usb_font_data.h"
#if !LV_USE_TINY_TTF
#error "USB UI requires CONFIG_LV_USE_TINY_TTF=y"
#endif
constexpr const char* kKeyHint = "Top 2s: Light | +/- 3s: USB";

namespace {
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
        ClearPanelMemory();
        const esp_err_t display_on = esp_lcd_panel_disp_on_off(panel_, true);
        if (display_on != ESP_ERR_NOT_SUPPORTED) ESP_ERROR_CHECK(display_on);
        // Brief pure-black phase helps distinguish retained LCD pixels from
        // physical image persistence. No LVGL objects exist at this point.
        vTaskDelay(pdMS_TO_TICKS(250));
        // GPIO46 is owned by UsbBacklight, including while audio is active.

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
        font_small_ = lv_tiny_ttf_create_data(kUsbFontData, sizeof(kUsbFontData), 16);
        font_body_ = lv_tiny_ttf_create_data(kUsbFontData, sizeof(kUsbFontData), 20);
        font_large_ = lv_tiny_ttf_create_data(kUsbFontData, sizeof(kUsbFontData), 38);
        ESP_ERROR_CHECK(font_small_ && font_body_ && font_large_ ? ESP_OK : ESP_ERR_NO_MEM);
        auto* screen = lv_display_get_screen_active(lcd);
        // Do not depend on a theme to supply an opaque background. In partial
        // RGB565 rendering, every dirty region must repaint its old pixels.
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(screen, lv_color_hex(kBackground), 0);
        lv_obj_set_style_text_font(screen, font_body_, 0);
        lv_obj_set_style_text_color(screen, lv_color_hex(kText), 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        // A single hideable local view: never leave labels/icons rendering
        // beneath the desktop canvas when its dirty regions are refreshed.
        local_root_ = lv_obj_create(screen);
        lv_obj_remove_style_all(local_root_);
        lv_obj_set_size(local_root_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_style_bg_opa(local_root_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(local_root_, lv_color_hex(kBackground), 0);
        lv_obj_remove_flag(local_root_, LV_OBJ_FLAG_SCROLLABLE);
        screen = local_root_;
        auto label = [this, screen](lv_align_t align, int y) {
            auto* result = lv_label_create(screen);
            lv_obj_set_width(result, LV_PCT(90));
            lv_obj_set_style_text_font(result, font_body_, 0);
            lv_label_set_long_mode(result, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(result, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_line_space(result, 2, 0);
            lv_obj_align(result, align, 0, y);
            return result;
        };
        auto* brand = lv_label_create(screen);
        lv_obj_set_style_text_font(brand, font_small_, 0);
        lv_obj_set_style_text_color(brand, lv_color_hex(kMuted), 0);
        lv_label_set_text(brand, "SPARKBOT");
        lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 16, 14);
        status_ = label(LV_ALIGN_TOP_RIGHT, 12);
        lv_obj_set_width(status_, 126);
        lv_obj_set_style_text_font(status_, font_small_, 0);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(status_, LV_ALIGN_TOP_RIGHT, -16, 14);
        lv_obj_set_style_text_color(status_, lv_color_hex(kAccent), 0);
        lv_label_set_text(status_, "USB Audio");
        InitializeContentPanel(screen);
        InitializeAudioPanel();
        footer_ = label(LV_ALIGN_BOTTOM_MID, -8);
        lv_obj_set_style_text_color(footer_, lv_color_hex(kMuted), 0);
        lv_obj_set_style_text_font(footer_, font_small_, 0);
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
        screen_canvas_ = lv_canvas_create(lv_display_get_screen_active(lcd));
        lv_obj_remove_style_all(screen_canvas_);
        lv_canvas_set_buffer(screen_canvas_, screen_pixels_, 240, 240, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(screen_canvas_, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_flag(screen_canvas_, LV_OBJ_FLAG_HIDDEN);
        ESP_ERROR_CHECK(UsbScreen::Initialize() ? ESP_OK : ESP_ERR_NO_MEM);
        // Full-screen, square and opaque: the settings page must not expose
        // an old volume icon (or desktop) around rounded card edges.
        brightness_panel_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(brightness_panel_);
        lv_obj_set_style_pad_all(brightness_panel_, 20, 0);
        lv_obj_set_size(brightness_panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_center(brightness_panel_);
        lv_obj_remove_flag(brightness_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(brightness_panel_, lv_color_hex(kSurface), 0);
        lv_obj_set_style_bg_opa(brightness_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(brightness_panel_, lv_color_hex(kAccent), 0);
        lv_obj_set_style_text_color(brightness_panel_, lv_color_hex(kText), 0);
        lv_obj_set_style_text_font(brightness_panel_, font_body_, 0);
        brightness_title_ = lv_label_create(brightness_panel_);
        lv_obj_align(brightness_title_, LV_ALIGN_TOP_MID, 0, 16);
        brightness_value_ = lv_label_create(brightness_panel_);
        lv_obj_set_style_text_font(brightness_value_, font_large_, 0);
        lv_obj_align(brightness_value_, LV_ALIGN_TOP_MID, 0, 62);
        brightness_bar_ = lv_bar_create(brightness_panel_);
        lv_obj_set_size(brightness_bar_, 170, 8);
        lv_obj_align(brightness_bar_, LV_ALIGN_TOP_MID, 0, 120);
        lv_bar_set_range(brightness_bar_, 0, 100);
        lv_obj_set_style_bg_color(brightness_bar_, lv_color_hex(kAccent), LV_PART_INDICATOR);
        auto* hint = lv_label_create(brightness_panel_);
        lv_obj_set_style_text_font(hint, font_small_, 0);
        lv_label_set_text(hint, "+/- Adjust   Top: Done");
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_flag(brightness_panel_, LV_OBJ_FLAG_HIDDEN);
        brightness_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto* self = static_cast<UsbDisplay*>(lv_timer_get_user_data(timer));
            lv_obj_add_flag(self->brightness_panel_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(lv_obj_get_screen(self->screen_canvas_));
            lv_timer_pause(timer);
        }, 1200, this);
        lv_timer_pause(brightness_timer_);
        InitializeVolumePopup();
        // Explicit initial full repaint after the complete object tree exists.
        lv_obj_invalidate(lv_display_get_screen_active(lcd));
    }

    void ShowVolume(int volume) {
        DisplayLockGuard lock(this);
        volume = std::clamp(volume, 0, 100);
        char text[16];
        snprintf(text, sizeof(text), "%d%%", volume);
        lv_label_set_text(volume_popup_value_, text);
        lv_label_set_text(volume_popup_title_, volume == 0 ? "Speaker muted" : "Speaker volume");
        lv_bar_set_value(volume_popup_bar_, volume, LV_ANIM_OFF);
        lv_obj_remove_flag(volume_popup_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(volume_popup_);
        lv_timer_reset(volume_popup_timer_);
        lv_timer_resume(volume_popup_timer_);
    }

    void ShowBrightness(int value, bool editing, bool saved = true) {
        DisplayLockGuard lock(this);
        lv_timer_pause(brightness_timer_);
        lv_obj_add_flag(volume_popup_, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(volume_popup_timer_);
        lv_label_set_text(brightness_title_, editing ? "Brightness" : saved ? "Brightness saved" : "Save failed");
        char text[16];
        snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(brightness_value_, text);
        lv_bar_set_value(brightness_bar_, value, LV_ANIM_OFF);
        lv_obj_remove_flag(brightness_panel_, LV_OBJ_FLAG_HIDDEN);
        if (!editing) {
            lv_timer_reset(brightness_timer_);
            lv_timer_resume(brightness_timer_);
        }
    }

    void UpdateUsbScreen() {
        DisplayLockGuard lock(this);
        const bool connected = UsbAudio::GetInstance().IsConnected();
        if (audio_view_ && connected && UsbScreen::TakeFrame(screen_pixels_)) {
            SetDesktopVisible(true);
            lv_obj_invalidate(screen_canvas_);
        }
        if (!audio_view_ || !connected) SetDesktopVisible(false);
    }

    void SetStatus(const char* text) override {
        DisplayLockGuard lock(this);
        lv_label_set_text(status_, text && text[0] ? text : "USB Audio");
        const bool audio = !text || !text[0] || std::strcmp(text, "USB Audio") == 0;
        if (audio != audio_view_) {
            // One content tree: destroy the previous view, never cover it.
            lv_obj_clean(audio_panel_);
            body_ = speaker_status_ = volume_bar_ = nullptr;
            mic_label_ = mic_level_bar_ = nullptr;
            for (auto& wave : speaker_waves_) wave = nullptr;
            displayed_volume_ = displayed_mic_ = -1;
            audio_view_ = audio;
            if (audio) {
                InitializeAudioPanel();
            } else {
                body_ = lv_label_create(audio_panel_);
                lv_obj_set_width(body_, 188);
                lv_obj_set_style_text_font(body_, font_body_, 0);
                lv_obj_set_style_text_color(body_, lv_color_hex(kText), 0);
                lv_obj_set_style_text_align(body_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_text_line_space(body_, 8, 0);
                lv_label_set_long_mode(body_, LV_LABEL_LONG_WRAP);
                lv_obj_center(body_);
            }
        }
        if (!audio) {
            lv_obj_add_flag(volume_popup_, LV_OBJ_FLAG_HIDDEN);
            lv_timer_pause(volume_popup_timer_);
            SetDesktopVisible(false);
            const bool debug = std::strcmp(text, "USB Debug") == 0;
            const bool error = std::strcmp(text, "USB Error") == 0;
            lv_label_set_text(body_, debug ?
                "Native Serial/JTAG\nHold +/- for 3s\nto return to audio" : error ?
                "USB initialization failed\nSwitching disabled\nPress RESET to recover" :
                "Switching USB mode\nHost will reconnect\nNo restart needed");
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

    void UpdateAudioState(int volume, bool mic_in_use, bool mic_muted = false,
                          int level = 0, int mic_level = 0) {
        DisplayLockGuard lock(this);
        if (!audio_view_) return;
        // Input remains live even when USB recording is idle or locally muted.
        lv_bar_set_value(volume_bar_, volume > 0 ? std::clamp(level, 0, 100) : 0, LV_ANIM_OFF);
        lv_bar_set_value(mic_level_bar_, std::clamp(mic_level, 0, 100), LV_ANIM_OFF);
        volume = std::clamp(volume, 0, 100);
        const int speaker_state = volume > 0 ? 1 : 0;
        if (displayed_volume_ != volume) {
            displayed_volume_ = volume;
            const int waves = SpeakerWaveCount(volume);
            for (int i = 0; i < 3; ++i) {
                if (i < waves) lv_obj_remove_flag(speaker_waves_[i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(speaker_waves_[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_label_set_text(speaker_status_, speaker_state ? "USB playback" : "Output muted");
            lv_obj_set_style_text_color(speaker_status_,
                lv_color_hex(speaker_state ? kMuted : kWarning), 0);
        }
        const int mic_state = !mic_in_use ? 0 : mic_muted ? 2 : 1;
        if (displayed_mic_ != mic_state) {
            displayed_mic_ = mic_state;
            lv_label_set_text(mic_label_, mic_state == 2 ? "Local input / USB muted" :
                mic_in_use ? "Local input / USB live" : "Local input / USB idle");
            lv_obj_set_style_text_color(mic_label_,
                lv_color_hex(mic_state == 2 ? kWarning : mic_in_use ? kSuccess : kMuted), 0);
        }
    }

protected:
    bool Lock(int timeout_ms) override { return lvgl_port_lock(timeout_ms); }
    void Unlock() override { lvgl_port_unlock(); }

private:
    void ClearPanelMemory() {
        // SPI DMA is asynchronous: wait for each stripe before reusing/freeing
        // its buffer. This runs before esp_lvgl_port installs its own callback.
        auto done = xSemaphoreCreateBinary();
        ESP_ERROR_CHECK(done ? ESP_OK : ESP_ERR_NO_MEM);
        auto* black = static_cast<uint16_t*>(heap_caps_calloc(
            DISPLAY_WIDTH * 20, sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        ESP_ERROR_CHECK(black ? ESP_OK : ESP_ERR_NO_MEM);
        const esp_lcd_panel_io_callbacks_t callbacks = {
            .on_color_trans_done = [](esp_lcd_panel_io_handle_t,
                                     esp_lcd_panel_io_event_data_t*, void* ctx) -> bool {
                BaseType_t wake = pdFALSE;
                xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(ctx), &wake);
                return wake == pdTRUE;
            },
        };
        ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(panel_io_, &callbacks, done));
        for (int y = 0; y < DISPLAY_HEIGHT; y += 20) {
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_, 0, y, DISPLAY_WIDTH,
                std::min(y + 20, DISPLAY_HEIGHT), black));
            ESP_ERROR_CHECK(xSemaphoreTake(done, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT);
        }
        const esp_lcd_panel_io_callbacks_t no_callbacks = {};
        ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(panel_io_, &no_callbacks, nullptr));
        heap_caps_free(black);
        vSemaphoreDelete(done);
    }

    void InitializeVolumePopup() {
        volume_popup_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(volume_popup_);
        lv_obj_set_size(volume_popup_, 204, 116);
        lv_obj_center(volume_popup_);
        lv_obj_set_style_bg_opa(volume_popup_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(volume_popup_, lv_color_hex(kSurface), 0);
        lv_obj_set_style_radius(volume_popup_, 14, 0);
        lv_obj_set_style_border_width(volume_popup_, 1, 0);
        lv_obj_set_style_border_color(volume_popup_, lv_color_hex(kAccent), 0);
        lv_obj_set_style_text_color(volume_popup_, lv_color_hex(kText), 0);
        lv_obj_remove_flag(volume_popup_, LV_OBJ_FLAG_SCROLLABLE);
        volume_popup_title_ = lv_label_create(volume_popup_);
        lv_obj_set_style_text_font(volume_popup_title_, font_body_, 0);
        lv_obj_align(volume_popup_title_, LV_ALIGN_TOP_MID, 0, 10);
        volume_popup_value_ = lv_label_create(volume_popup_);
        lv_obj_set_style_text_font(volume_popup_value_, font_large_, 0);
        lv_obj_align(volume_popup_value_, LV_ALIGN_TOP_MID, 0, 37);
        volume_popup_bar_ = lv_bar_create(volume_popup_);
        lv_obj_set_size(volume_popup_bar_, 168, 8);
        lv_obj_align(volume_popup_bar_, LV_ALIGN_BOTTOM_MID, 0, -14);
        lv_bar_set_range(volume_popup_bar_, 0, 100);
        lv_obj_set_style_bg_color(volume_popup_bar_, lv_color_hex(kAccent), LV_PART_INDICATOR);
        lv_obj_add_flag(volume_popup_, LV_OBJ_FLAG_HIDDEN);
        volume_popup_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto* self = static_cast<UsbDisplay*>(lv_timer_get_user_data(timer));
            lv_obj_add_flag(self->volume_popup_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(lv_obj_get_screen(self->screen_canvas_));
            lv_timer_pause(timer);
        }, 1800, this);
        lv_timer_pause(volume_popup_timer_);
    }
    lv_obj_t* volume_popup_ = nullptr;
    lv_obj_t* volume_popup_title_ = nullptr;
    lv_obj_t* volume_popup_value_ = nullptr;
    lv_obj_t* volume_popup_bar_ = nullptr;
    lv_timer_t* volume_popup_timer_ = nullptr;
    lv_obj_t* speaker_waves_[3] = {};

    // Called only with the LVGL lock held. Full invalidation clears previously
    // drawn pixels even when the next desktop update is only a small rectangle.
    void SetDesktopVisible(bool visible) {
        if (desktop_visible_ == visible) return;
        desktop_visible_ = visible;
        if (visible) {
            lv_obj_add_flag(local_root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(screen_canvas_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(screen_canvas_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(local_root_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_invalidate(lv_obj_get_screen(screen_canvas_));
    }
    bool desktop_visible_ = false;
    lv_obj_t* local_root_ = nullptr;
    lv_obj_t* brightness_panel_ = nullptr;
    lv_obj_t* brightness_title_ = nullptr;
    lv_obj_t* brightness_value_ = nullptr;
    lv_obj_t* brightness_bar_ = nullptr;
    lv_timer_t* brightness_timer_ = nullptr;
    lv_font_t* font_small_ = nullptr;
    lv_font_t* font_body_ = nullptr;
    lv_font_t* font_large_ = nullptr;
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
        lv_obj_set_style_bg_color(audio_panel_, lv_color_hex(kBackground), 0);
    }

    void InitializeAudioPanel() {
        // Two equal channels, with the same title, meter and status baselines.
        // No legacy volume percentage, large speaker or nested mic pill.
        auto card = [this](int y, const char* title) {
            auto* obj = lv_obj_create(audio_panel_);
            lv_obj_remove_style_all(obj);
            lv_obj_set_size(obj, 216, 80);
            lv_obj_set_pos(obj, 0, y);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(obj, lv_color_hex(kSurface), 0);
            lv_obj_set_style_radius(obj, 14, 0);
            lv_obj_set_style_border_width(obj, 1, 0);
            lv_obj_set_style_border_color(obj, lv_color_hex(kBorder), 0);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            auto* label = lv_label_create(obj);
            lv_obj_set_style_text_font(label, font_body_, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(kText), 0);
            lv_label_set_text(label, title);
            lv_obj_set_pos(label, 54, 8);
            return obj;
        };
        auto meter = [](lv_obj_t* parent, uint32_t color) {
            auto* bar = lv_bar_create(parent);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, 184, 8);
            lv_obj_set_pos(bar, 16, 42);
            lv_bar_set_range(bar, 0, 100);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(kBorder), LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
            lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
            return bar;
        };
        auto status = [this](lv_obj_t* parent, const char* text) {
            auto* label = lv_label_create(parent);
            lv_obj_set_style_text_font(label, font_small_, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(kMuted), 0);
            lv_label_set_text(label, text);
            lv_obj_set_pos(label, 16, 56);
            return label;
        };
        auto line = [](lv_obj_t* parent, const lv_point_precise_t* points,
                       uint32_t count, uint32_t color) {
            auto* obj = lv_line_create(parent);
            lv_line_set_points(obj, points, count);
            lv_obj_set_style_line_width(obj, 2, 0);
            lv_obj_set_style_line_rounded(obj, true, 0);
            lv_obj_set_style_line_color(obj, lv_color_hex(color), 0);
        };
        auto arc = [](lv_obj_t* parent, int x, int y, int size, int start,
                      int end, uint32_t color) {
            auto* obj = lv_arc_create(parent);
            lv_obj_remove_style_all(obj);
            lv_obj_set_size(obj, size, size);
            lv_obj_set_pos(obj, x, y);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            lv_arc_set_bg_angles(obj, 0, 0);
            lv_arc_set_angles(obj, start, end);
            lv_obj_set_style_arc_width(obj, 2, LV_PART_INDICATOR);
            lv_obj_set_style_arc_rounded(obj, true, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(obj, lv_color_hex(color), LV_PART_INDICATOR);
            lv_obj_set_style_arc_opa(obj, LV_OPA_COVER, LV_PART_INDICATOR);
            return obj;
        };
        auto* speaker = card(0, "Speaker");
        static const lv_point_precise_t speaker_outline[] = {
            {15, 18}, {20, 18}, {27, 12}, {27, 32}, {20, 26}, {15, 26}, {15, 18}
        };
        line(speaker, speaker_outline, 7, kAccent);
        for (int i = 0; i < 3; ++i) {
            const int radius = 6 + i * 4;
            speaker_waves_[i] = arc(speaker, 26 - radius, 22 - radius,
                radius * 2, 320, 40, kAccent);
            lv_obj_add_flag(speaker_waves_[i], LV_OBJ_FLAG_HIDDEN);
        }
        volume_bar_ = meter(speaker, kAccent);
        speaker_status_ = status(speaker, "USB playback");

        auto* mic = card(86, "Microphone");
        auto* capsule = lv_obj_create(mic);
        lv_obj_remove_style_all(capsule);
        lv_obj_set_pos(capsule, 20, 10);
        lv_obj_set_size(capsule, 8, 16);
        lv_obj_set_style_radius(capsule, 4, 0);
        lv_obj_set_style_border_width(capsule, 2, 0);
        lv_obj_set_style_border_color(capsule, lv_color_hex(kSuccess), 0);
        lv_obj_remove_flag(capsule, LV_OBJ_FLAG_SCROLLABLE);
        arc(mic, 15, 11, 18, 0, 180, kSuccess);
        static const lv_point_precise_t mic_stand[] = {
            {24, 29}, {24, 33}, {19, 33}, {29, 33}
        };
        line(mic, mic_stand, 4, kSuccess);
        mic_level_bar_ = meter(mic, kSuccess);
        mic_label_ = status(mic, "Local input / USB idle");
    }

    bool audio_view_ = true;  // Protected by the LVGL display lock.
    lv_obj_t* audio_panel_ = nullptr;
    lv_obj_t* speaker_status_ = nullptr;
    lv_obj_t* volume_bar_ = nullptr;
    lv_obj_t* mic_label_ = nullptr;
    lv_obj_t* mic_level_bar_ = nullptr;
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
        backlight_.Initialize(DISPLAY_BACKLIGHT_PIN);
        brightness_keys_.Initialize(backlight_.Saved());
        display_.Initialize();
        codec_ = new Es8311AudioCodec(i2c_bus_, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, AUDIO_CODEC_ES8311_ADDR); // Shared PA/BL pin owned by PWM.
        ESP_ERROR_CHECK(bridge_.Initialize(codec_) ? ESP_OK : ESP_FAIL);
        ESP_ERROR_CHECK(bridge_.Start() ? ESP_OK : ESP_FAIL);
        vTaskDelay(pdMS_TO_TICKS(150)); // Initial shared PA enable settling time.
        backlight_.Set(brightness_keys_.Value());
        display_.UpdateAudioState(codec_->output_volume(), false);
        OtgMode::GetInstance().Begin(&display_);
        InitializeKeys();
    }

public:
    void PollKeys() {
        {
            std::lock_guard<std::mutex> lock(keys_mutex_);
            ApplyBrightnessAction(brightness_keys_.Poll(esp_timer_get_time()));
            if (!brightness_keys_.Editing()) combo_.Poll();
        }
        display_.UpdateUsbScreen();
        const int64_t now = esp_timer_get_time();
        if (now - last_ui_update_us_ < 50000) return;
        last_ui_update_us_ = now;
        int volume;
        {
            std::lock_guard<std::mutex> lock(volume_mutex_);
            volume = codec_->output_volume();
        }
        display_.UpdateAudioState(volume, UsbAudio::GetInstance().IsMicrophoneActive(),
            UsbAudio::GetInstance().IsMicrophoneMuted(), bridge_.PlaybackLevel(), bridge_.MicrophoneLevel());
    }

private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    UsbDisplay display_;
    Es8311AudioCodec* codec_ = nullptr;
    UsbAudioBridge bridge_;
    ComboKeyDetector combo_;
    UsbBacklight backlight_;
    BrightnessKeys brightness_keys_;
    std::mutex keys_mutex_;
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
        auto& usb = UsbAudio::GetInstance();
        if (usb.IsMicrophoneActive()) {
            if (usb.ToggleMicrophoneMute()) {
                display_.ShowNotification(usb.IsMicrophoneMuted() ? "Mic muted" : "Mic unmuted", 1500);
            }
            return;
        }
        const bool queued = usb.RequestPlayPause();
        display_.ShowNotification(queued ? "Play/Pause requested" : "Media control unavailable", 1500);
    }

    void ChangeVolume(int delta) {
        int volume;
        {
            std::lock_guard<std::mutex> lock(volume_mutex_);
            codec_->SetOutputVolume(std::clamp(codec_->output_volume() + delta, 0, 100));
            volume = codec_->output_volume();
        }
        display_.ShowVolume(volume);
    }

    // Called with keys_mutex_: touch callbacks and polling share one state machine.
    void ApplyBrightnessAction(int action) {
        if (action & (BrightnessKeys::Refresh | BrightnessKeys::Closed)) combo_.Reset();
        if (action & BrightnessKeys::Refresh) {
            backlight_.Set(brightness_keys_.Value());
            display_.ShowBrightness(brightness_keys_.Value(), true);
        }
        if (action & BrightnessKeys::Closed) {
            const bool saved = backlight_.Save(brightness_keys_.Value());
            display_.ShowBrightness(brightness_keys_.Value(), false, saved);
        }
        if ((action & BrightnessKeys::Media) && !combo_.IsComboActive()) PlayPause();
    }

    void OnTouch(int id, bool down) {
        std::lock_guard<std::mutex> lock(keys_mutex_);
        ApplyBrightnessAction(brightness_keys_.Poll(esp_timer_get_time()));
        if (brightness_keys_.IsDown(id) == down) return;
        const bool was_editing = brightness_keys_.Editing();
        const int action = brightness_keys_.Key(id, down, esp_timer_get_time());
        ApplyBrightnessAction(action);
        if (id == 0 || was_editing || brightness_keys_.Editing()) return;
        if (down) combo_.PressDown(id);
        else {
            const bool chord = combo_.IsComboActive();
            combo_.PressUp(id);
            if (!chord) ChangeVolume(id == 1 ? -10 : 10);
        }
    }

    void InitializeKeys() {
        combo_.OnCombo([](uint32_t mask) { OtgMode::GetInstance().ToggleByKeys(mask); });
        boot_.OnPressDown([this]() { combo_.PressDown(COMBO_KEY_BOOT); });
        boot_.OnPressUp([this]() { combo_.PressUp(COMBO_KEY_BOOT); });
        boot_.OnLongPress([this]() {
            std::lock_guard<std::mutex> lock(keys_mutex_);
            if (!brightness_keys_.Editing() && !combo_.IsComboActive())
                OtgMode::GetInstance().ToggleByKeys();
        });
        const int pads[] = {TOUCH_BUTTON_PAD_1, TOUCH_BUTTON_PAD_2, TOUCH_BUTTON_PAD_3};
        const float sensitivity[] = {TOUCH_BUTTON_SENSITIVITY_1,
            TOUCH_BUTTON_SENSITIVITY_2, TOUCH_BUTTON_SENSITIVITY_3};
        for (int i = 0; i < TOUCH_BUTTON_NUM; ++i) {
            touch_[i] = new TouchButton(pads[i], sensitivity[i]);
            touch_[i]->OnPress([this, i]() { OnTouch(i, true); });
            touch_[i]->OnRelease([this, i]() { OnTouch(i, false); });
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
