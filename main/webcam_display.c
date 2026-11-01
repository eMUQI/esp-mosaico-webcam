// SPDX-License-Identifier: Apache-2.0
#include "webcam_display.h"
#include "bsp/display.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_display_present.h"
#include "esp_lv_present.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "webcam_display";
static SemaphoreHandle_t display_mutex, rotation_mutex, rotation_done, init_done;
static TaskHandle_t display_task;
static esp_display_presenter_t *presenter;
static lv_display_t *display;
static lv_indev_t *input_device;
static esp_lcd_touch_handle_t touch;
static unsigned active_rotation, requested_rotation;
static bool rotation_pending, display_fault;
static esp_err_t init_result, rotation_result;

bool webcam_display_lock(int timeout_ms)
{
    return display_mutex && xSemaphoreTakeRecursive(display_mutex,
        timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void webcam_display_unlock(void)
{
    xSemaphoreGiveRecursive(display_mutex);
}

lv_indev_t *webcam_display_get_input_dev(void)
{
    return input_device;
}

unsigned webcam_display_get_rotation(void)
{
    if (!webcam_display_lock(-1)) return 0;
    unsigned rotation = active_rotation;
    webcam_display_unlock();
    return rotation;
}

static void read_touch(lv_indev_t *device, lv_indev_data_t *data)
{
    (void)device;
    data->state = LV_INDEV_STATE_RELEASED;
    if (!touch || esp_lcd_touch_read_data(touch) != ESP_OK) return;
    esp_lcd_touch_point_data_t point;
    uint8_t points = 0;
    if (esp_lcd_touch_get_data(touch, &point, &points, 1) != ESP_OK || !points) return;
    int32_t logical_x = point.x, logical_y = point.y;
    if (esp_display_presenter_map_point(presenter, &logical_x, &logical_y) != ESP_OK) return;
    data->point = (lv_point_t){logical_x, logical_y};
    data->state = LV_INDEV_STATE_PRESSED;
}

static esp_err_t rotate_display(unsigned rotation)
{
    if (rotation == active_rotation) return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_display_presenter_quiesce(presenter, 1000), TAG, "drain display");
    esp_err_t result = esp_display_presenter_set_rotation(presenter,
        (esp_display_present_rotation_t)rotation);
    esp_display_presenter_drawbuf_t buffers = {0};
    esp_err_t err = esp_display_presenter_get_drawbuf(presenter, &buffers);
    if (err != ESP_OK || !buffers.buffers[0] || !buffers.bytes || buffers.bytes > UINT32_MAX) {
        display_fault = true;
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    // Rotation rebuilds the pool; the square display keeps the binding's dimensions unchanged.
    lv_display_set_buffers(display, buffers.buffers[0], buffers.count > 1 ? buffers.buffers[1] : NULL,
        buffers.bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    if (result == ESP_OK) active_rotation = rotation;
    if (input_device) {
        lv_indev_reset(input_device, NULL);
        lv_indev_wait_release(input_device);
    }
    lv_obj_invalidate(lv_display_get_screen_active(display));
    return result;
}

esp_err_t webcam_display_set_rotation(unsigned rotation)
{
    if (rotation > 270 || rotation % 90) return ESP_ERR_INVALID_ARG;
    if (!display_task || xTaskGetCurrentTaskHandle() == display_task) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(rotation_mutex, portMAX_DELAY);
    webcam_display_lock(-1);
    if (display_fault) {
        webcam_display_unlock();
        xSemaphoreGive(rotation_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (rotation == active_rotation) {
        webcam_display_unlock();
        xSemaphoreGive(rotation_mutex);
        return ESP_OK;
    }
    requested_rotation = rotation;
    rotation_pending = true;
    webcam_display_unlock();
    xSemaphoreTake(rotation_done, portMAX_DELAY);
    esp_err_t result = rotation_result;
    xSemaphoreGive(rotation_mutex);
    return result;
}

static esp_err_t initialize_display(void)
{
    const bsp_display_config_t panel_config = {.rotation = BSP_DISPLAY_ROTATE_0};
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(bsp_display_new(&panel_config, &panel), TAG, "panel init");
    const esp_display_presenter_config_t config = {
        .width = BSP_LCD_H_RES, .height = BSP_LCD_V_RES,
        .pixel_format = ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
        .max_damage_areas = 32,
        .target = {
            .hw = {
                .panel = panel, .io = bsp_display_get_panel_io(),
                .panel_type = ESP_DISPLAY_PRESENT_PANEL_IO,
                .input_pixel_format = ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
                .rotation = ESP_DISPLAY_PRESENT_ROTATE_0, .swap_bytes = true,
                .te_enabled = true,
                .te_sync = {.gpio_num = BSP_LCD_TE, .bus_freq_hz = BSP_LCD_PIXEL_CLOCK_HZ,
                            .data_lines = BSP_LCD_DATA_WIDTH},
            },
            .fb = {.mode = ESP_DISPLAY_PRESENT_MODE_TE_SYNC},
            // One tile buffer keeps LVGL reuse synchronous; TE retains two full-frame buffers.
            .drawbuf = {.lines = 32, .buffers = 1, .in_psram = true, .te_compose_buffers = 2},
        },
    };
    ESP_RETURN_ON_ERROR(esp_display_presenter_create(&config, &presenter), TAG, "presenter init");
    esp_err_t err = esp_lv_present_start(presenter, &display);
    if (err != ESP_OK) {
        (void)esp_display_presenter_delete(presenter);
        presenter = NULL;
        return err;
    }
    lv_obj_set_style_bg_color(lv_display_get_screen_active(display), lv_color_black(), 0);
    err = bsp_touch_new(BSP_DISPLAY_ROTATE_0, &touch);
    if (err == ESP_OK) {
        input_device = lv_indev_create();
        if (!input_device) return ESP_ERR_NO_MEM;
        lv_indev_set_type(input_device, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(input_device, display);
        lv_indev_set_read_cb(input_device, read_touch);
    } else {
        ESP_LOGW(TAG, "Touch unavailable: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void display_worker(void *arg)
{
    (void)arg;
    webcam_display_lock(-1);
    init_result = initialize_display();
    if (init_result != ESP_OK) display_fault = true;
    webcam_display_unlock();
    xSemaphoreGive(init_done);
    if (init_result != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        webcam_display_lock(-1);
        if (rotation_pending) {
            rotation_result = rotate_display(requested_rotation);
            rotation_pending = false;
            xSemaphoreGive(rotation_done);
        }
        esp_display_presenter_state_t state;
        if (esp_display_presenter_get_state(presenter, &state) != ESP_OK ||
            state != ESP_DISPLAY_PRESENTER_STATE_OPEN) {
            if (!display_fault) ESP_LOGE(TAG, "Display presenter faulted; rendering paused");
            display_fault = true;
        }
        uint32_t delay_ms = display_fault ? 5 : lv_timer_handler();
        webcam_display_unlock();
        if (delay_ms > 5) delay_ms = 5;
        vTaskDelay(pdMS_TO_TICKS(delay_ms ? delay_ms : 1));
    }
}

esp_err_t webcam_display_start(void)
{
    if (display_task) return ESP_ERR_INVALID_STATE;
    display_mutex = xSemaphoreCreateRecursiveMutex();
    rotation_mutex = xSemaphoreCreateMutex();
    rotation_done = xSemaphoreCreateBinary();
    init_done = xSemaphoreCreateBinary();
    if (!display_mutex || !rotation_mutex || !rotation_done || !init_done) {
        if (display_mutex) vSemaphoreDelete(display_mutex);
        if (rotation_mutex) vSemaphoreDelete(rotation_mutex);
        if (rotation_done) vSemaphoreDelete(rotation_done);
        if (init_done) vSemaphoreDelete(init_done);
        display_mutex = rotation_mutex = rotation_done = init_done = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(display_worker, "display", 6144, NULL, 4, &display_task, 0) != pdPASS) {
        vSemaphoreDelete(display_mutex);
        vSemaphoreDelete(rotation_mutex);
        vSemaphoreDelete(rotation_done);
        vSemaphoreDelete(init_done);
        display_mutex = rotation_mutex = rotation_done = init_done = NULL;
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(init_done, portMAX_DELAY);
    vSemaphoreDelete(init_done);
    init_done = NULL;
    return init_result;
}
