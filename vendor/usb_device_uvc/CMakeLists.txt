if(${IDF_VERSION_MAJOR} LESS 6)
    set(usb_requires usb)
else()
    set(usb_requires esp_hw_support)
endif()

idf_component_register(SRCS usb_device_uvc.c
                    INCLUDE_DIRS "include"
                    REQUIRES ${usb_requires} esp_timer)

idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(tusb_dir espressif__tinyusb COMPONENT_DIR)

# Patch a build-local copy so managed component integrity remains unchanged.
set(video_source "${tusb_dir}/src/class/video/video_device.c")
set(video_patched "${CMAKE_CURRENT_BINARY_DIR}/video_device.c")
set(video_patch "${COMPONENT_DIR}/tusb/video-device-recovery.patch")
configure_file("${video_source}" "${video_patched}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${video_patch}")
find_program(UVC_GIT git REQUIRED)
execute_process(COMMAND "${UVC_GIT}" apply --no-index "${video_patch}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                RESULT_VARIABLE patch_result ERROR_VARIABLE patch_error)
if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "TinyUSB video recovery patch failed: ${patch_error}")
endif()
set_source_files_properties("${video_source}" TARGET_DIRECTORY ${tusb_lib}
                            PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(${tusb_lib} PRIVATE "${video_patched}")
target_include_directories(${tusb_lib} PRIVATE "${tusb_dir}/src/class/video" "${COMPONENT_DIR}/include")

target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb")
target_sources(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb/usb_descriptors.c")

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
