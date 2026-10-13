#include "usb_cdc.h"

#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include "tinyusb.h"
#include "tusb.h"
#include "tusb_cdc_acm.h"

#define TAG "UsbCdc"

static bool s_installed = false;

static const tusb_desc_device_t kDescDevice = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_CDC,                // pure CDC
    .bDeviceSubClass    = CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL,
    .bDeviceProtocol    = CDC_COMM_PROTOCOL_ATCOMMAND,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,                        // same Espressif VID
    .idProduct          = 0x4012,                        // different PID for CDC-only
    .bcdDevice          = 0x0101,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// CDC ACM configuration descriptor: 1 communication + 1 data interface
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
static const uint8_t kDescConfig[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};
static_assert(sizeof(kDescConfig) == TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN,
              "CDC descriptor size mismatch");

static const char kLanguage[] = {0x09, 0x04};
static const char* kStrings[] = {
    kLanguage, "ESP32-S3", "ESP32-S3 USB CDC (Console)", "0001", "CDC ACM",
};

bool UsbCdcIsInstalled() { return s_installed; }

bool UsbCdcInstall() {
    if (s_installed) return true;

    const tinyusb_config_t tusb_config = {
        .device_descriptor = &kDescDevice,
        .string_descriptor = kStrings,
        .string_descriptor_count = sizeof(kStrings) / sizeof(kStrings[0]),
        .external_phy = false,
        .configuration_descriptor = kDescConfig,
        .self_powered = false,
        .vbus_monitor_io = -1,
    };

    const esp_err_t err = tinyusb_driver_install(&tusb_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install(cdc): %s", esp_err_to_name(err));
        return false;
    }
    s_installed = true;
    ESP_LOGI(TAG, "USB CDC (console) installed");
    return true;
}

void UsbCdcUninstall() {
    if (!s_installed) return;
    tinyusb_driver_uninstall();
    s_installed = false;
    ESP_LOGI(TAG, "USB CDC uninstalled");
}

void UsbCdcPrint(const char* text) {
    if (!text || !s_installed) return;
    // 简单的行缓冲：找到 \n 之前的内容发出去
    const char* nl = strchr(text, '\n');
    size_t len = nl ? (size_t)(nl - text) : strlen(text);
    if (len == 0) return;
    tud_cdc_n_write(0, text, len);
    tud_cdc_n_write_flush(0);
}
