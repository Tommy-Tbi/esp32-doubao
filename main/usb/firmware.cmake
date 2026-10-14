if(NOT CONFIG_USB_AUDIO_DEVICE)
    message(FATAL_ERROR "SparkBot USB firmware requires USB_AUDIO_DEVICE=y")
endif()

idf_build_get_property(BUILD_COMPONENTS BUILD_COMPONENTS)

# Anchor paths to this included file, independent of the path resolution used
# by idf_component_register() and its helpers.
set(FIRMWARE_USB_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(FIRMWARE_ROOT "${FIRMWARE_USB_DIR}/.." ABSOLUTE)

idf_component_register(
    SRCS
        "${FIRMWARE_ROOT}/main.cc"
        "${FIRMWARE_USB_DIR}/usb_device.cc"
        "${FIRMWARE_USB_DIR}/usb_audio_bridge.cc"
        "${FIRMWARE_ROOT}/usb_audio.cc"
        "${FIRMWARE_ROOT}/audio/audio_codec.cc"
        "${FIRMWARE_ROOT}/audio/codecs/es8311_audio_codec.cc"
        "${FIRMWARE_ROOT}/display/display.cc"
        "${FIRMWARE_ROOT}/settings.cc"
        "${FIRMWARE_ROOT}/boards/common/button.cc"
        "${FIRMWARE_ROOT}/boards/esp-sparkbot/sparkbot_keys.cc"
        "${FIRMWARE_ROOT}/boards/esp-sparkbot/otg_mode.cc"
    INCLUDE_DIRS
        "${FIRMWARE_ROOT}"
        "${FIRMWARE_ROOT}/audio"
        "${FIRMWARE_ROOT}/display"
        "${FIRMWARE_ROOT}/display/lvgl_display"
        "${FIRMWARE_ROOT}/boards/common"
        "${FIRMWARE_ROOT}/boards/esp-sparkbot"
        "${FIRMWARE_USB_DIR}"
)

include("${FIRMWARE_USB_DIR}/audio_config.cmake")
