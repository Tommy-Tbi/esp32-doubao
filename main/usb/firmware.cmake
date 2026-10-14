if(NOT CONFIG_USB_AUDIO_DEVICE)
    message(FATAL_ERROR "SparkBot USB firmware requires USB_AUDIO_DEVICE=y")
endif()

idf_component_register(
    SRCS
        "main.cc"
        "usb/usb_device.cc"
        "usb/usb_audio_bridge.cc"
        "usb_audio.cc"
        "audio/audio_codec.cc"
        "audio/codecs/es8311_audio_codec.cc"
        "display/display.cc"
        "settings.cc"
        "boards/common/button.cc"
        "boards/esp-sparkbot/sparkbot_keys.cc"
        "boards/esp-sparkbot/otg_mode.cc"
    INCLUDE_DIRS "." "audio" "display" "display/lvgl_display"
                 "boards/common" "boards/esp-sparkbot" "usb"
)

idf_build_get_property(BUILD_COMPONENTS BUILD_COMPONENTS)
include(${CMAKE_CURRENT_LIST_DIR}/audio_config.cmake)

# The UI uses the built-in font; no assistant assets/model partition is needed.
idf_build_get_property(PROJECT_APP_BIN APP_BIN)
add_custom_target(package_firmware ALL
    COMMAND ${PYTHON} ${PROJECT_DIR}/scripts/package_firmware.py -b ${CMAKE_BINARY_DIR}
    DEPENDS ${APP_BIN}
    COMMENT "Packaging standalone USB firmware"
    VERBATIM
)
add_dependencies(package_firmware app)
