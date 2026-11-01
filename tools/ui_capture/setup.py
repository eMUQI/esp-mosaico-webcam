"""Prepare the native LVGL renderer with production configuration and hardware substitutes."""
from pathlib import Path
import shutil
import sys

source = Path(__file__).resolve().parent
root = source.parents[1]
work = Path(sys.argv[1]).resolve()
stubs = work / "stubs"
stubs.mkdir(parents=True, exist_ok=True)
for name in ("bsp/display.h", "bsp/imu.h", "driver/ppa.h", "esp_cache.h", "esp_check.h",
             "esp_heap_caps.h", "esp_log.h", "esp_timer.h", "freertos/FreeRTOS.h",
             "freertos/semphr.h", "freertos/task.h", "esp_err.h", "usb_device_uvc.h"):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "host.h"\n')
shutil.copyfile(source / "host.h", stubs / "host.h")
shutil.copyfile(source / "mosaico_module_mgr.h", stubs / "mosaico_module_mgr.h")
shutil.copyfile(root / "build/config/sdkconfig.h", stubs / "sdkconfig.h")
shutil.copyfile(source / "capture.c", work / "capture.c")
(work / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(webcam_language_capture C)
add_compile_options(-g -UNDEBUG -fsanitize=address,undefined)
add_link_options(-fsanitize=address,undefined)
file(GLOB_RECURSE LV_SOURCES "{root}/managed_components/lvgl__lvgl/src/*.c")
add_library(lvgl STATIC ${{LV_SOURCES}})
target_include_directories(lvgl PUBLIC "{root}/managed_components/lvgl__lvgl" "{root}/managed_components/lvgl__lvgl/src" "{stubs}")
target_compile_definitions(lvgl PUBLIC LV_CONF_SKIP LV_CONF_KCONFIG_EXTERNAL_INCLUDE="sdkconfig.h")
add_executable(capture capture.c "{root}/main/ui_assets.c" "{root}/main/ui_font_14.c" "{root}/main/ui_font_20.c")
target_include_directories(capture PRIVATE "{root}/main")
target_link_libraries(capture PRIVATE lvgl)
''')
