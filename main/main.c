// SPDX-License-Identifier: Apache-2.0
#include "webcam.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "webcam";

static esp_err_t start_webcam(void)
{
    // Preserve existing NVS data if its format is incompatible with this firmware.
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS initialization");
    ESP_RETURN_ON_ERROR(bsp_power_init(), TAG, "board power");
    ESP_RETURN_ON_ERROR(pipeline_init(), TAG, "pipeline initialization");
    uint8_t *transfer = heap_caps_malloc(JPEG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(transfer, ESP_ERR_NO_MEM, TAG, "USB transfer buffer");
    uvc_device_config_t cfg = {
        .uvc_buffer = transfer, .uvc_buffer_size = JPEG_CAPACITY,
        .start_cb = pipeline_uvc_start, .stop_cb = pipeline_uvc_stop,
        .fb_get_cb = pipeline_uvc_get, .fb_return_cb = pipeline_uvc_return,
    };
    ESP_RETURN_ON_ERROR(uvc_device_config(0, &cfg), TAG, "UVC configuration");
    ESP_RETURN_ON_ERROR(uvc_device_init(), TAG, "UVC initialization");
    ESP_RETURN_ON_ERROR(preview_ui_init(), TAG, "display initialization");
    return pipeline_start_tasks();
}

void app_main(void)
{
    esp_err_t err = start_webcam();
    if (err == ESP_OK) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        // Vibe Mode leaves a newly installed application pending until startup succeeds.
        if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
            (state == ESP_OTA_IMG_NEW || state == ESP_OTA_IMG_PENDING_VERIFY)) {
            err = esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    if (err != ESP_OK) {
        // Keep the failure visible on UART without entering a restart loop.
        while (true) {
            ESP_LOGE(TAG, "Startup stopped: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    ESP_LOGI(TAG, "VGA MJPEG %u fps target; privacy is enabled at every boot", FRAME_FPS);
}
