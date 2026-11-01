// SPDX-License-Identifier: Apache-2.0
#include "webcam.h"
#include "frame_policy.h"
#include <assert.h>
#include <string.h>
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mosaico_module_camera.h"
#include "nvs.h"
#include "tusb.h"

static const char *TAG = "pipeline";
enum slot_state { FREE, WRITING, READY, BORROWED };
typedef struct {
    uvc_fb_t fb;
    enum slot_state state;
    uint32_t epoch;
    uint64_t sequence;
    int64_t captured_us;
    size_t capacity;
} jpeg_slot_t;

static SemaphoreHandle_t lock;
static webcam_status_t status;
static jpeg_slot_t slots[3];
static uvc_fb_t black;
typedef struct {
    uint8_t *data;
    size_t capacity;
    uint32_t width, height;
    unsigned rotation;
    uint16_t x, y, content_width, content_height;
} raw_slot_t;
static raw_slot_t raw_slots[2];
static raw_slot_t *raw;
static const void *preview_raw;
static jpeg_encoder_handle_t encoder;
static ppa_client_handle_t ppa;
static uint64_t sequence;
static uint64_t last_fed_sequence;
static bool settings_dirty;

static void stamp_frame_timestamp(uvc_fb_t *fb)
{
    int64_t us = esp_timer_get_time();
    fb->timestamp.tv_sec = us / 1000000;
    fb->timestamp.tv_usec = us % 1000000;
}

void pipeline_snapshot(webcam_status_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(lock);
    out->mounted = tud_mounted();
    out->streaming = out->mounted && !tud_suspended() && tud_video_n_streaming(0, 0);
    out->high_speed = tud_speed_get() == TUSB_SPEED_HIGH;
}

void pipeline_privacy(bool enabled)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    status.privacy = enabled;
    ++status.epoch;
    for (unsigned i = 0; i < 3; ++i) {
        if (slots[i].state == READY) slots[i].state = FREE;
    }
    xSemaphoreGive(lock);
}

void pipeline_screen_off(bool off)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    status.screen_off = off;
    xSemaphoreGive(lock);
}

void pipeline_settings(const webcam_settings_t *settings)
{
    if (settings->rotation > 270 || settings->rotation % 90 ||
        (unsigned)settings->language > WEBCAM_LANGUAGE_EN) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (settings->rotation != status.settings.rotation) {
        // Reject queued and in-progress frames produced with the previous orientation.
        ++status.epoch;
        for (unsigned i = 0; i < 3; ++i) {
            if (slots[i].state == READY) slots[i].state = FREE;
        }
    }
    status.settings = *settings;
    settings_dirty = true;
    xSemaphoreGive(lock);
}

void pipeline_preview_result(bool rendered, uint32_t elapsed_us)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (rendered) {
        ++status.previewed;
        status.preview_us = elapsed_us;
    }
    else ++status.preview_drops;
    xSemaphoreGive(lock);
}

void pipeline_preview_release(const void *rgb565)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    assert(preview_raw == rgb565);
    preview_raw = NULL;
    xSemaphoreGive(lock);
}

static void camera_error(esp_err_t error)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    status.camera_ok = false;
    status.error = error;
    for (unsigned i = 0; i < 3; ++i) {
        if (slots[i].state == READY) slots[i].state = FREE;
    }
    xSemaphoreGive(lock);
    ESP_LOGW(TAG, "Camera unavailable: %s", esp_err_to_name(error));
}

static jpeg_slot_t *reserve_slot(void)
{
    jpeg_slot_t *chosen = NULL;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (unsigned i = 0; i < 3; ++i) {
        if (slots[i].state == FREE) { chosen = &slots[i]; break; }
        if (slots[i].state == READY && (!chosen || slots[i].sequence < chosen->sequence)) {
            chosen = &slots[i];
        }
    }
    if (chosen) {
        if (chosen->state == READY) ++status.replaced;
        chosen->state = WRITING;
    }
    xSemaphoreGive(lock);
    return chosen;
}

esp_err_t pipeline_uvc_start(uvc_format_t format, int width, int height, int rate, void *ctx)
{
    (void)ctx;
    if (format != UVC_FORMAT_JPEG || width != FRAME_WIDTH || height != FRAME_HEIGHT || rate != FRAME_FPS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    ++status.epoch;
    for (unsigned i = 0; i < 3; ++i) {
        if (slots[i].state == READY) slots[i].state = FREE;
    }
    xSemaphoreGive(lock);
    return ESP_OK;
}

void pipeline_uvc_stop(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(lock, portMAX_DELAY);
    ++status.epoch;
    for (unsigned i = 0; i < 3; ++i) {
        if (slots[i].state == READY) slots[i].state = FREE;
    }
    xSemaphoreGive(lock);
}

uvc_fb_t *pipeline_uvc_get(void *ctx)
{
    (void)ctx;
    jpeg_slot_t *latest = NULL;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!status.privacy && status.camera_ok && !CONFIG_WEBCAM_STATIC_FRAME) {
        for (unsigned i = 0; i < 3; ++i) {
            if (slots[i].state == READY && slots[i].epoch == status.epoch &&
                esp_timer_get_time() - slots[i].captured_us < 500000 &&
                (!latest || slots[i].sequence > latest->sequence)) latest = &slots[i];
        }
    }
    if (latest) {
        latest->state = BORROWED;
        status.frame_age_us = esp_timer_get_time() - latest->captured_us;
        if (latest->sequence != last_fed_sequence) ++status.fresh;
        else ++status.repeated;
        last_fed_sequence = latest->sequence;
    } else {
        ++status.black_frames;
    }
    ++status.fed;
    uvc_fb_t *fb = latest ? &latest->fb : &black;
    stamp_frame_timestamp(fb);
    xSemaphoreGive(lock);
    return fb;
}

void pipeline_uvc_return(uvc_fb_t *fb, void *ctx)
{
    (void)ctx;
    if (fb == &black) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (unsigned i = 0; i < 3; ++i) {
        if (fb == &slots[i].fb) {
            assert(slots[i].state == BORROWED);
            slots[i].state = frame_publish_allowed(status.privacy, status.epoch, slots[i].epoch) ? READY : FREE;
            break;
        }
    }
    xSemaphoreGive(lock);
}

static esp_err_t encode_jpeg_frame(uint8_t *output, size_t capacity, uint32_t *size)
{
    const jpeg_encode_cfg_t cfg = {
        .width = FRAME_WIDTH, .height = FRAME_HEIGHT,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422, .image_quality = 80,
    };
    esp_err_t err = jpeg_encoder_process(encoder, &cfg, raw->data, RAW_BYTES, output, capacity, size);
    if (err == ESP_OK && (*size < 4 || *size > capacity || output[0] != 0xff || output[1] != 0xd8 ||
                          output[*size - 2] != 0xff || output[*size - 1] != 0xd9)) return ESP_ERR_INVALID_RESPONSE;
    return err;
}

static esp_err_t normalize_camera_frame(const mosaico_camera_frame_t *frame, unsigned rotation)
{
    unsigned scale16, x, y;
    const uint32_t stride = frame->bytes_per_line ? frame->bytes_per_line : frame->width * 2U;
    if (frame->pixel_format != V4L2_PIX_FMT_UYVY || !frame->data ||
        stride < frame->width * 2U || stride % 2 || frame->size < (size_t)stride * frame->height ||
        !frame_fit(frame->width, frame->height, rotation, FRAME_WIDTH, FRAME_HEIGHT, &scale16, &x, &y)) {
        return ESP_ERR_INVALID_SIZE;
    }
    // Clear letterboxing only when geometry changes; PPA overwrites the active area.
    if (raw->width != frame->width || raw->height != frame->height || raw->rotation != rotation) {
        memset(raw->data, 0, RAW_BYTES);
        ESP_RETURN_ON_ERROR(esp_cache_msync(raw->data, raw->capacity, ESP_CACHE_MSYNC_FLAG_DIR_C2M), TAG, "raw cache");
        raw->width = frame->width;
        raw->height = frame->height;
        raw->rotation = rotation;
        raw->x = x;
        raw->y = y;
        raw->content_width = (rotation % 180 ? frame->height : frame->width) * scale16 / 16;
        raw->content_height = (rotation % 180 ? frame->width : frame->height) * scale16 / 16;
    }
    ESP_RETURN_ON_ERROR(esp_cache_msync((void *)frame->data, (size_t)stride * frame->height,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE), TAG, "camera cache");
    const ppa_srm_rotation_angle_t rotations[] = {
        PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
        PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270,
    };
    const ppa_srm_oper_config_t op = {
        .in = {.buffer = frame->data, .pic_w = stride / 2, .pic_h = frame->height,
               .block_w = frame->width, .block_h = frame->height,
               .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
               .yuv_range = PPA_COLOR_RANGE_LIMIT, .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .out = {.buffer = raw->data, .buffer_size = raw->capacity,
                .pic_w = FRAME_WIDTH, .pic_h = FRAME_HEIGHT,
                .block_offset_x = x, .block_offset_y = y, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = rotations[rotation / 90],
        .scale_x = scale16 / 16.0f, .scale_y = scale16 / 16.0f,
        // Compose the USB horizontal reflection with the existing mounting correction.
        .mirror_x = !CONFIG_WEBCAM_SENSOR_MIRROR_X,
        .mirror_y = CONFIG_WEBCAM_SENSOR_MIRROR_Y, .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(ppa, &op), TAG, "normalize");
    return esp_cache_msync(raw->data, raw->capacity, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

static void save_settings(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool dirty = settings_dirty;
    webcam_settings_t settings = status.settings;
    settings_dirty = false;
    xSemaphoreGive(lock);
    if (!dirty) return;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("webcam", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        uint8_t values[] = {3, settings.brightness, settings.preview, settings.mirror,
                            settings.sleep_seconds & 255, settings.sleep_seconds >> 8,
                            settings.rotation / 90, settings.language};
        err = nvs_set_blob(nvs, "settings", values, sizeof(values));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Settings were not saved: %s", esp_err_to_name(err));
}

static void capture_task(void *arg)
{
    (void)arg;
    mosaico_camera_handle_t camera = NULL;
    bool asleep = false;
    bool cleanup_pending = false;
    unsigned failures = 0;
    unsigned warmup = 0;
    while (true) {
        webcam_status_t current;
        pipeline_snapshot(&current);
        if (cleanup_pending) {
            esp_err_t err = mosaico_camera_del(camera);
            if (err != ESP_OK) { camera_error(err); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
            camera = NULL;
            cleanup_pending = false;
            asleep = false;
        }
        const bool preview = current.settings.preview && !current.screen_off;
        const bool needed = !current.privacy && !CONFIG_WEBCAM_STATIC_FRAME && (current.streaming || preview);
        if (!needed) {
            if (camera && !asleep) {
                esp_err_t err = mosaico_camera_set_power_down(camera, true);
                if (err != ESP_OK) { camera_error(err); cleanup_pending = true; }
                else asleep = true;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        if (!camera) {
            mosaico_camera_config_t cfg = MOSAICO_CAMERA_DEFAULT_CONFIG();
            cfg.frame_timeout_ms = 250;
            cfg.buffer_count = 1;
            esp_err_t err = mosaico_camera_new(&cfg, &camera);
            if (err == ESP_OK) err = mosaico_camera_open(camera);
            mosaico_camera_info_t info = {0};
            if (err == ESP_OK) err = mosaico_camera_get_info(camera, &info);
            if (err == ESP_OK && info.width == 640 && info.height == 480) {
                err = mosaico_camera_del(camera);
                if (err == ESP_OK) {
                    camera = NULL;
                    cfg.buffer_count = 2;
                    err = mosaico_camera_new(&cfg, &camera);
                    if (err == ESP_OK) err = mosaico_camera_open(camera);
                    if (err == ESP_OK) err = mosaico_camera_get_info(camera, &info);
                }
            }
            if (err == ESP_OK) err = mosaico_camera_start_stream(camera);
            if (err != ESP_OK) {
                camera_error(err);
                cleanup_pending = camera != NULL;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            failures = 0;
            warmup = 5;
            xSemaphoreTake(lock, portMAX_DELAY);
            status.sensor_fps = info.frame_rate;
            xSemaphoreGive(lock);
            ESP_LOGI(TAG, "Camera %lux%lu sensor=%lu fps buffers=%u; USB target=%u fps",
                     (unsigned long)info.width, (unsigned long)info.height,
                     (unsigned long)info.frame_rate, info.buffer_count, FRAME_FPS);
        }
        if (asleep) {
            esp_err_t err = mosaico_camera_set_power_down(camera, false);
            if (err != ESP_OK) { camera_error(err); cleanup_pending = true; continue; }
            asleep = false;
            warmup = 5;
        }
        mosaico_camera_frame_t frame = {0};
        esp_err_t err = mosaico_camera_get_frame(camera, &frame);
        if (err != ESP_OK) {
            xSemaphoreTake(lock, portMAX_DELAY);
            if (err == ESP_ERR_INVALID_SIZE) ++status.incomplete;
            else ++status.capture_errors;
            xSemaphoreGive(lock);
            // The BSP requeues incomplete frames; rebuilding the camera would interrupt valid frames.
            if (err == ESP_ERR_INVALID_SIZE) {
                vTaskDelay(1);
                continue;
            }
            if (err == ESP_ERR_TIMEOUT) {
                err = mosaico_camera_restart(camera);
                xSemaphoreTake(lock, portMAX_DELAY);
                ++status.restarts;
                xSemaphoreGive(lock);
                if (err == ESP_OK) { warmup = 5; failures = 0; continue; }
                camera_error(err);
                cleanup_pending = true;
                continue;
            }
            if (++failures >= 3) { camera_error(err); cleanup_pending = true; }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        failures = 0;
        xSemaphoreTake(lock, portMAX_DELAY);
        ++status.captured;
        status.outstanding = 1;
        raw = &raw_slots[preview_raw == raw_slots[0].data ? 1 : 0];
        assert(raw->data != preview_raw);
        xSemaphoreGive(lock);
        const int64_t now = esp_timer_get_time();
        const bool ready = warmup == 0;
        if (warmup) --warmup;
        if (ready) err = normalize_camera_frame(&frame, current.settings.rotation);
        const uint32_t normalize_us = esp_timer_get_time() - now;
        // A failed return retains ownership; never restart or free a borrowed DMA frame.
        esp_err_t return_err;
        while ((return_err = mosaico_camera_return_frame(camera, &frame)) != ESP_OK) {
            camera_error(return_err);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        status.outstanding = 0;
        if (ready) status.normalize_us = normalize_us;
        xSemaphoreGive(lock);
        if (!ready) continue;
        if (err != ESP_OK) { camera_error(err); vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        jpeg_slot_t *slot = current.streaming ? reserve_slot() : NULL;
        uint32_t size = 0;
        int64_t start = esp_timer_get_time();
        if (slot) err = encode_jpeg_frame(slot->fb.buf, slot->capacity, &size);
        xSemaphoreTake(lock, portMAX_DELAY);
        if (err == ESP_OK) {
            status.camera_ok = true;
            status.error = ESP_OK;
        } else {
            ++status.encode_errors;
            status.error = err;
            status.camera_ok = false;
        }
        if (slot) {
            status.encode_us = esp_timer_get_time() - start;
            if (err == ESP_OK && frame_publish_allowed(status.privacy, status.epoch, current.epoch)) {
                slot->fb.len = size;
                slot->epoch = current.epoch;
                slot->sequence = ++sequence;
                slot->captured_us = now;
                slot->state = READY;
                status.jpeg_bytes = size;
                ++status.encoded;
            } else slot->state = FREE;
        }
        const bool may_preview = preview && !preview_raw &&
                                 frame_publish_allowed(status.privacy, status.epoch, current.epoch);
        if (may_preview) preview_raw = raw->data;
        xSemaphoreGive(lock);
        if (may_preview) {
            const webcam_preview_frame_t preview_frame = {
                .data = raw->data, .epoch = current.epoch,
                .x = raw->x, .y = raw->y, .width = raw->content_width, .height = raw->content_height,
                .video_rotation = raw->rotation,
            };
            if (!preview_ui_submit(&preview_frame)) {
                pipeline_preview_release(raw->data);
                pipeline_preview_result(false, 0);
            }
        } else if (preview) {
            pipeline_preview_result(false, 0);
        }
        vTaskDelay(1);
    }
}

static void maintenance_task(void *arg)
{
    (void)arg;
    webcam_status_t previous = {0};
    int64_t previous_us = esp_timer_get_time();
    for (unsigned seconds = 0;; ++seconds) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        save_settings();
        if (CONFIG_WEBCAM_DIAGNOSTICS && seconds % 5 == 4) {
            webcam_status_t current;
            pipeline_snapshot(&current);
            const int64_t now = esp_timer_get_time();
            const double elapsed = (now - previous_us) / 1000000.0;
            ESP_LOGI(TAG, "capture=%.1f encoded=%.1f feed=%.1f fresh=%.1f repeat=%.1f black=%.1f preview=%.1f fps sensor=%lu jpeg=%luB normalize_us=%lu encode_us=%lu preview_us=%lu age_us=%lu replaced=%lu incomplete=%lu restarts=%lu capture_err=%lu encode_err=%lu preview_drop=%lu outstanding=%u heap=%u min=%u psram=%u speed=%s",
                     (current.captured - previous.captured) / elapsed,
                     (current.encoded - previous.encoded) / elapsed,
                     (current.fed - previous.fed) / elapsed,
                     (current.fresh - previous.fresh) / elapsed,
                     (current.repeated - previous.repeated) / elapsed,
                     (current.black_frames - previous.black_frames) / elapsed,
                     (current.previewed - previous.previewed) / elapsed,
                     (unsigned long)current.sensor_fps, (unsigned long)current.jpeg_bytes,
                     (unsigned long)current.normalize_us, (unsigned long)current.encode_us,
                     (unsigned long)current.preview_us, (unsigned long)current.frame_age_us,
                     (unsigned long)current.replaced, (unsigned long)current.incomplete,
                     (unsigned long)current.restarts, (unsigned long)current.capture_errors,
                     (unsigned long)current.encode_errors, (unsigned long)current.preview_drops,
                     current.outstanding, (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), current.high_speed ? "HS" : "FS");
            previous = current;
            previous_us = now;
        }
    }
}

esp_err_t pipeline_init(void)
{
    lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(lock, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_FALSE(CONFIG_WEBCAM_ROTATION % 90 == 0, ESP_ERR_INVALID_ARG, TAG, "rotation");
    status.privacy = true;
    status.settings = (webcam_settings_t){.brightness = 70, .preview = true, .mirror = true,
                                         .sleep_seconds = WEBCAM_DEFAULT_SLEEP_SECONDS,
                                         .rotation = CONFIG_WEBCAM_ROTATION, .language = WEBCAM_LANGUAGE_ZH};
    nvs_handle_t nvs;
    if (nvs_open("webcam", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t values[8];
        size_t size = sizeof(values);
        if (nvs_get_blob(nvs, "settings", values, &size) == ESP_OK) {
            (void)webcam_settings_decode(&status.settings, values, size);
        }
        nvs_close(nvs);
    }
    const jpeg_encode_memory_alloc_cfg_t in_mem = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
    const jpeg_encode_memory_alloc_cfg_t out_mem = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    for (unsigned i = 0; i < 2; ++i) {
        raw_slots[i].data = jpeg_alloc_encoder_mem(RAW_BYTES, &in_mem, &raw_slots[i].capacity);
        ESP_RETURN_ON_FALSE(raw_slots[i].data, ESP_ERR_NO_MEM, TAG, "raw buffer");
    }
    raw = &raw_slots[0];
    for (unsigned i = 0; i < 3; ++i) {
        slots[i].fb = (uvc_fb_t){.width = FRAME_WIDTH, .height = FRAME_HEIGHT, .format = UVC_FORMAT_JPEG};
        slots[i].fb.buf = jpeg_alloc_encoder_mem(JPEG_CAPACITY, &out_mem, &slots[i].capacity);
        ESP_RETURN_ON_FALSE(slots[i].fb.buf, ESP_ERR_NO_MEM, TAG, "JPEG buffer");
    }
    const jpeg_encode_engine_cfg_t engine_cfg = {.timeout_ms = 200};
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&engine_cfg, &encoder), TAG, "JPEG engine");
    memset(raw->data, 0, RAW_BYTES);
    uint32_t black_size;
    ESP_RETURN_ON_ERROR(encode_jpeg_frame(slots[0].fb.buf, slots[0].capacity, &black_size), TAG, "black JPEG");
    black = slots[0].fb;
    black.buf = heap_caps_malloc(black_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(black.buf, ESP_ERR_NO_MEM, TAG, "black buffer");
    black.len = black_size;
    memcpy(black.buf, slots[0].fb.buf, black_size);
    const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    return ppa_register_client(&ppa_cfg, &ppa);
}

esp_err_t pipeline_start_tasks(void)
{
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(capture_task, "capture", 8192, NULL, 5, NULL, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "capture task");
    ESP_RETURN_ON_FALSE(xTaskCreate(maintenance_task, "diagnostics", 4096, NULL, 1, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "maintenance task");
    return ESP_OK;
}
