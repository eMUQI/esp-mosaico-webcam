cmake_minimum_required(VERSION 3.22)
set(PROJECT_VER "0.1.0-dev")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Retain the official AI-button and ABI 2 recovery selection policy.
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/vendor/esp-mosaico-utils/esp-mosaico-vibe-mode/firmware/vibe-mode/bootloader_components" APPEND)
project(mosaico-webcam)
