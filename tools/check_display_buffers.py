#!/usr/bin/env python3
"""Run real LVGL and esp_lv_present with deliberately delayed tile consumption."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
work = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build-host-display"
source = (root / "main/webcam_display.c").read_text()
drawbuf = re.search(r"\.drawbuf\s*=\s*\{([^}]+)\}", source).group(1)
configured_count = int(re.search(r"\.buffers\s*=\s*(\d+)", drawbuf).group(1))
assert configured_count == 1, "Shared tile buffers must not enable asynchronous consumption"
assert ".te_compose_buffers = 2" in drawbuf

subprocess.run([sys.executable, str(root / "tools/ui_capture/setup.py"),
                str(work)], check=True)
stubs = work / "stubs"
headers = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_INVALID_SIZE 5
static inline const char *esp_err_to_name(int e) { (void)e; return "injected error"; }
""",
    "esp_lcd_panel_io.h": "#pragma once\ntypedef void *esp_lcd_panel_io_handle_t;\n",
    "esp_lcd_panel_ops.h": "#pragma once\ntypedef void *esp_lcd_panel_handle_t;\n",
    "esp_check.h": """#pragma once
#define ESP_RETURN_ON_FALSE(ok,err,...) do { if (!(ok)) return (err); } while(0)
#define ESP_RETURN_ON_ERROR(expr,...) do { int e=(expr); if(e) return e; } while(0)
""",
    "esp_log.h": "#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nstatic inline int64_t esp_timer_get_time(void) { return 0; }\n",
}
for name, text in headers.items():
    (stubs / name).write_text(text)

cmake = work / "CMakeLists.txt"
text = cmake.read_text().split("add_executable(", 1)[0]
text += f'''
add_executable(check "{root}/tools/display_buffer_fixture.c"
    "{root}/managed_components/espressif__esp_lv_present/src/esp_lv_present.c")
target_include_directories(check PRIVATE
    "{root}/managed_components/espressif__esp_lv_present/include"
    "{root}/managed_components/espressif__esp_display_present/include")
target_link_libraries(check PRIVATE lvgl)
'''
cmake.write_text(text)
with (work / "build.log").open("w") as log:
    subprocess.run(["cmake", "-S", str(work), "-B", str(work / "build")],
                    stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(["cmake", "--build", str(work / "build"), "-j", "8"],
                    stdout=log, stderr=subprocess.STDOUT, check=True)
for count in (2, configured_count):
    subprocess.run([str(work / "build/check"), str(count)], check=True)
