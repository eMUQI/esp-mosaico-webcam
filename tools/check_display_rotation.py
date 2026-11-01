#!/usr/bin/env python3
"""Check production draw-buffer rebinding when the presenter changes rotation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/webcam_display.c").read_text()
start = source.index("static esp_err_t rotate_display(")
end = source.index("\nesp_err_t webcam_display_set_rotation(", start)
production = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_display_present_geometry.h"
typedef struct { void *buffers[2]; unsigned count; size_t bytes; } esp_display_presenter_drawbuf_t;
#define LV_DISPLAY_RENDER_MODE_PARTIAL 1
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e = (expr); if (e) return e; } while (0)
static int presenter, display, input_device = 1;
static unsigned active_rotation;
static bool display_fault;
static int drain_error, rotate_error, buffer_error;
static char calls[16];
static unsigned cursor;
static uint8_t first[32];
static esp_display_presenter_drawbuf_t pool;
static void record(char c) { calls[cursor++] = c; calls[cursor] = 0; }
static int esp_display_presenter_quiesce(int p, int timeout)
{ (void)p; assert(timeout == 1000); record('D'); return drain_error; }
static int esp_display_presenter_set_rotation(int p, unsigned angle)
{ (void)p; assert(angle % 90 == 0); record('R'); return rotate_error; }
static int esp_display_presenter_get_drawbuf(int p, esp_display_presenter_drawbuf_t *out)
{ (void)p; record('G'); *out = pool; return buffer_error; }
static void lv_display_set_buffers(int d, void *a, void *b, uint32_t bytes, int mode)
{ (void)d; assert(a == first && b == NULL && bytes == sizeof(first));
  assert(mode == LV_DISPLAY_RENDER_MODE_PARTIAL); record('B'); }
static void lv_indev_reset(int i, void *o) { (void)i; (void)o; record('T'); }
static void lv_indev_wait_release(int i) { (void)i; record('W'); }
static int lv_display_get_screen_active(int d) { return d; }
static void lv_obj_invalidate(int d) { (void)d; record('I'); }
/* PRODUCTION */
static void reset(void)
{
    active_rotation = 0; display_fault = false;
    drain_error = rotate_error = buffer_error = 0;
    cursor = 0; calls[0] = 0;
    pool = (esp_display_presenter_drawbuf_t){{first, NULL}, 1, sizeof(first)};
}
int main(void)
{
    reset(); assert(rotate_display(0) == ESP_OK && !calls[0]);
    for (unsigned angle = 90; angle <= 270; angle += 90) {
        reset(); assert(rotate_display(angle) == ESP_OK);
        assert(active_rotation == angle && !display_fault);
        assert(strcmp(calls, "DRGBTWI") == 0);
    }
    reset(); drain_error = -2;
    assert(rotate_display(90) == -2 && active_rotation == 0);
    assert(strcmp(calls, "D") == 0);
    reset(); rotate_error = -3;
    assert(rotate_display(90) == -3 && active_rotation == 0 && !display_fault);
    assert(strcmp(calls, "DRGBTWI") == 0);
    reset(); pool.buffers[0] = NULL;
    assert(rotate_display(90) == ESP_ERR_INVALID_STATE && display_fault);
    assert(strcmp(calls, "DRG") == 0);
    reset(); buffer_error = -4;
    assert(rotate_display(90) == -4 && display_fault);
    assert(strcmp(calls, "DRG") == 0);
    for (unsigned angle = 0; angle < 360; angle += 90) {
        for (int x = 0; x < 480; x += 13) for (int y = 0; y < 480; y += 17) {
            esp_display_present_point_t physical = {x, y}, logical, restored;
            const esp_display_present_size_t size = {480, 480};
            assert(esp_display_present_geometry_map_physical_point_to_logical(
                angle, size, &physical, &logical) == ESP_OK);
            int expected_x = angle == 90 ? y : angle == 180 ? 479-x : angle == 270 ? 479-y : x;
            int expected_y = angle == 90 ? 479-x : angle == 180 ? 479-y : angle == 270 ? x : y;
            assert(logical.x == expected_x && logical.y == expected_y);
            assert(esp_display_present_geometry_map_logical_point_to_physical(
                angle, size, &logical, &restored) == ESP_OK);
            assert(restored.x == x && restored.y == y);
        }
    }
    puts("Rotation drains transfers, rebinds replacement buffers on success/rollback, resets input and rejects an unusable pool.");
    puts("Four-orientation image/touch transforms round-trip and preserve the existing axis convention.");
}
'''
with tempfile.TemporaryDirectory(prefix="webcam-display-check-") as directory:
    work = Path(directory)
    component = root / "managed_components/espressif__esp_display_present"
    (work / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n"
        "#define ESP_OK 0\n#define ESP_ERR_INVALID_STATE -1\n#define ESP_ERR_INVALID_ARG -2\n")
    (work / "check.c").write_text(fixture.replace("/* PRODUCTION */", production))
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(work),
                    "-I", str(component / "include"), str(work / "check.c"),
                    str(component / "src/pixel/present_geometry.c"),
                    "-o", str(work / "check")], check=True)
    subprocess.run([str(work / "check")], check=True)
