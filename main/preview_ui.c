// SPDX-License-Identifier: Apache-2.0
#include "webcam.h"
#include "frame_policy.h"
#include "orientation_policy.h"
#include "ui_assets.h"
#include "mosaico_module_mgr.h"
#include <stdio.h>
#include <string.h>
#include "bsp/display.h"
#include "webcam_display.h"
#include "bsp/imu.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"

static const char *TAG = "preview";
static SemaphoreHandle_t mailbox;
static webcam_preview_frame_t input;
static uint8_t *output, *front;
static unsigned display_rotation;
static uint64_t output_geometry = UINT64_MAX, front_geometry = UINT64_MAX;
static TaskHandle_t preview_task_handle;
static bool pending;
static ppa_client_handle_t ppa;
static lv_obj_t *image, *state_label, *mode_label, *privacy_label, *notice;
static lv_obj_t *settings_panel, *wake_cover, *preview_switch, *mirror_switch;
static lv_obj_t *brightness_slider, *brightness_value, *sleep_dropdown, *rotation_dropdown;
static lv_obj_t *camera_art;
static lv_obj_t *camera_guide, *guide_arrow, *guide_title, *guide_hint;
static lv_obj_t *preview_value, *mirror_value, *language_buttons;
static lv_obj_t *title_label, *settings_label, *screen_off_label, *back_label;
static lv_obj_t *settings_title, *brightness_label, *preview_label, *preview_hint;
static lv_obj_t *mirror_label, *rotation_label, *rotation_hint, *sleep_label;
static webcam_language_t language;
static int64_t last_activity;
LV_FONT_DECLARE(webcam_font_20);
LV_FONT_DECLARE(webcam_font_14);

static lv_image_dsc_t descriptor;
static int brightness_applied = -1;
static bool preview_valid;
static bool guide_waiting_for_preview;

enum {
    UI_BACKGROUND = 0x000000,
    UI_SURFACE = 0x181819,
    UI_TEXT = 0xfcfcff,
    UI_MUTED = 0x91919b,
    UI_ACCENT = 0xff4c01,
    UI_WARNING = 0xfda711,
    UI_ERROR = 0xff0000,
    UI_SUCCESS = 0x63c270,
};

static void refresh(lv_timer_t *timer);

static mosaico_module_presence_t camera_presence(void)
{
    mosaico_module_mgr_info_t module;
    if (mosaico_module_mgr_get_info(MOSAICO_MODULE_MGR_SLOT_LEFT, &module) != ESP_OK) {
        return MOSAICO_MODULE_PRESENCE_UNKNOWN;
    }
    return module.presence;
}

static void guide_arrow_step(void *obj, int32_t elapsed)
{
    // Preserve the board-edge angle during the 2.2-second translation and opacity cycle.
    const int32_t shift = -4 + 8 * (elapsed < 1500 ? elapsed : 1500) / 1500;
    lv_obj_set_pos(obj, 199 + shift, 8 + shift * 268 / 1000);
    const int32_t opacity = elapsed < 250 ? elapsed * 255 / 250 :
                            elapsed < 1250 ? 255 :
                            elapsed < 1700 ? (1700 - elapsed) * 255 / 450 : 0;
    lv_obj_set_style_opa(obj, opacity, 0);
}

static void show_camera_guide(bool visible)
{
    if (visible == !lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN)) return;
    if (visible) {
        lv_obj_remove_flag(camera_guide, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, guide_arrow);
        lv_anim_set_exec_cb(&animation, guide_arrow_step);
        lv_anim_set_values(&animation, 0, 2200);
        lv_anim_set_duration(&animation, 2200);
        lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&animation);
    } else {
        lv_anim_delete(guide_arrow, guide_arrow_step);
        lv_obj_add_flag(camera_guide, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *translated(const char *chinese, const char *english)
{
    return language == WEBCAM_LANGUAGE_ZH ? chinese : english;
}

static void apply_language(webcam_language_t selected)
{
    language = selected;
    lv_label_set_text(title_label, translated("摄像头", "Webcam"));
    lv_label_set_text(settings_label, translated("设置", "Settings"));
    lv_label_set_text(screen_off_label, translated("关闭屏幕\n暂停预览", "Screen Off\nPause preview"));
    lv_label_set_text(settings_title, translated("设置", "Settings"));
    lv_label_set_text(brightness_label, translated("亮度", "Brightness"));
    lv_label_set_text(preview_label, translated("本地预览", "Local preview"));
    lv_label_set_text(preview_hint, translated("关闭后，USB 视频继续传输", "Off pauses preview; USB video continues"));
    lv_label_set_text(mirror_label, translated("预览镜像", "Preview mirror"));
    lv_label_set_text(rotation_label, translated("USB 视频旋转", "USB video rotation"));
    lv_label_set_text(rotation_hint, translated("本地预览随设备方向调整", "Local preview follows device orientation"));
    lv_label_set_text(sleep_label, translated("自动关屏", "Screen timeout"));
    lv_label_set_text(back_label, translated("返回", "Back"));
    lv_label_set_text(guide_title, translated("将摄像头插入左侧 H2", "Insert the camera into left H2"));
    lv_label_set_text(guide_hint, translated("对准接口，轻推到位", "Align the connector, then push gently"));
    uint32_t timeout = lv_dropdown_get_selected(sleep_dropdown);
    lv_dropdown_set_options(sleep_dropdown, translated("从不\n30 秒\n1 分钟\n5 分钟", "Never\n30 seconds\n1 minute\n5 minutes"));
    lv_dropdown_set_selected(sleep_dropdown, timeout);
    lv_buttonmatrix_set_button_ctrl(language_buttons, selected, LV_BUTTONMATRIX_CTRL_CHECKED);
}

static void language_changed(lv_event_t *event)
{
    uint32_t selected = lv_buttonmatrix_get_selected_button(lv_event_get_target(event));
    if (selected > WEBCAM_LANGUAGE_EN) return;
    last_activity = esp_timer_get_time();
    if (selected == (uint32_t)language) return;
    webcam_status_t state;
    pipeline_snapshot(&state);
    state.settings.language = (webcam_language_t)selected;
    pipeline_settings(&state.settings);
    apply_language(state.settings.language);
    refresh(NULL);
}

bool preview_ui_submit(const webcam_preview_frame_t *frame)
{
    if (!frame || !frame->data || !frame->width || !frame->height ||
        frame->x + frame->width > FRAME_WIDTH || frame->y + frame->height > FRAME_HEIGHT ||
        frame->video_rotation > 270 || frame->video_rotation % 90) return false;
    if (!mailbox || xSemaphoreTake(mailbox, 0) != pdTRUE) return false;
    if (pending) { xSemaphoreGive(mailbox); return false; }
    input = *frame;
    pending = true;
    xSemaphoreGive(mailbox);
    xTaskNotifyGive(preview_task_handle);
    return true;
}

static void preview_frame(void)
{
    // A consumed notification must drain its frame even if rotation owns the mailbox.
    if (xSemaphoreTake(mailbox, portMAX_DELAY) != pdTRUE) return;
    if (!pending) { xSemaphoreGive(mailbox); return; }
    pending = false;
    const webcam_preview_frame_t frame = input;
    const unsigned panel_rotation = display_rotation;
    xSemaphoreGive(mailbox);
    const uint32_t epoch = frame.epoch;
    const int64_t start = esp_timer_get_time();
    webcam_status_t state;
    pipeline_snapshot(&state);
    if (!frame_publish_allowed(state.privacy, state.epoch, epoch) || state.screen_off || !state.settings.preview) {
        pipeline_preview_release(frame.data);
        pipeline_preview_result(false, 0);
        return;
    }
    unsigned scale16, x, y;
    const unsigned rotation = preview_rotation(frame.video_rotation, panel_rotation,
                                               CONFIG_WEBCAM_SENSOR_MIRROR_X, CONFIG_WEBCAM_SENSOR_MIRROR_Y);
    if (!frame_fit(frame.width, frame.height, rotation, PREVIEW_WIDTH, PREVIEW_HEIGHT,
                   &scale16, &x, &y)) {
        pipeline_preview_release(frame.data);
        pipeline_preview_result(false, 0);
        return;
    }
    esp_err_t err = ESP_OK;
    const uint64_t geometry = ((uint64_t)frame.width << 32) | ((uint64_t)frame.height << 16) | rotation;
    if (output_geometry != geometry) {
        memset(output, 0, PREVIEW_BYTES);
        err = esp_cache_msync(output, PREVIEW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (err == ESP_OK) output_geometry = geometry;
    }
    const ppa_srm_rotation_angle_t rotations[] = {
        PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
        PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270,
    };
    const ppa_srm_oper_config_t op = {
        .in = {.buffer = frame.data, .pic_w = FRAME_WIDTH, .pic_h = FRAME_HEIGHT,
               .block_w = frame.width, .block_h = frame.height, .block_offset_x = frame.x, .block_offset_y = frame.y,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = output, .buffer_size = PREVIEW_BYTES,
                .pic_w = PREVIEW_WIDTH, .pic_h = PREVIEW_HEIGHT,
                .block_offset_x = x, .block_offset_y = y, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = rotations[rotation / 90],
        .scale_x = scale16 / 16.0f, .scale_y = scale16 / 16.0f,
        // Undo the USB reflection in panel coordinates before applying the local mirror preference.
        .mirror_x = state.settings.mirror != (rotation % 180 == 0),
        .mirror_y = rotation % 180 != 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (err == ESP_OK) err = ppa_do_scale_rotate_mirror(ppa, &op);
    pipeline_preview_release(frame.data);
    if (err == ESP_OK) err = esp_cache_msync(output, PREVIEW_BYTES,
                                           ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    bool rendered = false;
    if (err == ESP_OK && xSemaphoreTake(mailbox, 0) == pdTRUE) {
        // Rotation may complete while PPA runs; only publish for the sampled panel angle.
        if (display_rotation == panel_rotation && webcam_display_lock(5)) {
            pipeline_snapshot(&state);
            if (frame_publish_allowed(state.privacy, state.epoch, epoch) && state.error == ESP_OK &&
                !state.screen_off && state.settings.preview &&
                camera_presence() != MOSAICO_MODULE_PRESENCE_ABSENT) {
                lv_image_cache_drop(&descriptor);
                const bool full_refresh = !preview_valid || front_geometry != geometry;
                uint8_t *previous = front;
                front = output;
                output = previous;
                uint64_t previous_geometry = front_geometry;
                front_geometry = output_geometry;
                output_geometry = previous_geometry;
                descriptor.data = front;
                if (full_refresh) {
                    lv_obj_invalidate(image);
                } else {
                    // The descriptor address is stable; only the active image pixels change.
                    lv_area_t changed;
                    lv_obj_get_coords(image, &changed);
                    changed.x1 += x;
                    changed.y1 += y;
                    changed.x2 = changed.x1 + (rotation % 180 ? frame.height : frame.width) * scale16 / 16 - 1;
                    changed.y2 = changed.y1 + (rotation % 180 ? frame.width : frame.height) * scale16 / 16 - 1;
                    lv_obj_invalidate_area(image, &changed);
                }
                lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(camera_art, LV_OBJ_FLAG_HIDDEN);
                guide_waiting_for_preview = false;
                show_camera_guide(false);
                if (lv_label_get_text(notice)[0]) lv_label_set_text(notice, "");
                preview_valid = true;
                rendered = true;
            }
            webcam_display_unlock();
        }
        xSemaphoreGive(mailbox);
    }
    pipeline_preview_result(rendered, esp_timer_get_time() - start);
}

static void preview_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        preview_frame();
    }
}

static void orientation_task(void *arg)
{
    (void)arg;
    while (true) {
        esp_err_t err = bsp_imu_init();
        if (err == ESP_OK) err = bsp_imu_start(NULL);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "Auto orientation unavailable: %s; retrying in five seconds", esp_err_to_name(err));
        if (bsp_imu_deinit() != ESP_OK) {
            ESP_LOGW(TAG, "IMU cleanup failed; keeping the current display orientation");
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    orientation_policy_t policy = {0};
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        webcam_status_t state;
        pipeline_snapshot(&state);
        if (state.screen_off) {
            policy = (orientation_policy_t){0};
            continue;
        }
        float ax, ay, az;
        if (bsp_imu_get_accel(&ax, &ay, &az) != ESP_OK) {
            policy = (orientation_policy_t){0};
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        unsigned requested;
        if (!orientation_update(&policy, ax, ay, display_rotation, &requested)) continue;
        if (xSemaphoreTake(mailbox, pdMS_TO_TICKS(10)) != pdTRUE) continue;
        // The display task drains transfers before changing the image and touch transforms.
        const unsigned previous = display_rotation;
        esp_err_t err = webcam_display_set_rotation(requested);
        display_rotation = webcam_display_get_rotation();
        if (display_rotation != previous && webcam_display_lock(-1)) {
            lv_indev_t *touch = webcam_display_get_input_dev();
            if (touch) {
                // Consume touches begun while the panel and touch transforms were changing.
                lv_indev_reset(touch, NULL);
                lv_indev_wait_release(touch);
            }
            // Retain the last preview until a frame for the new orientation is published.
            webcam_display_unlock();
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "Display rotation failed: %s", esp_err_to_name(err));
        xSemaphoreGive(mailbox);
    }
}

static void clicked(lv_event_t *event)
{
    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    webcam_status_t state;
    pipeline_snapshot(&state);
    last_activity = esp_timer_get_time();
    if (action == 0) {
        pipeline_privacy(!state.privacy);
        preview_valid = false;
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    } else if (action == 1) {
        lv_obj_remove_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
    } else if (action == 2) {
        lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
    } else if (action == 3) {
        pipeline_screen_off(true);
        lv_obj_remove_flag(wake_cover, LV_OBJ_FLAG_HIDDEN);
    } else if (action == 4) {
        pipeline_screen_off(false);
        lv_obj_add_flag(wake_cover, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_changed(lv_event_t *event)
{
    last_activity = esp_timer_get_time();
    lv_label_set_text_fmt(brightness_value, "%d%%", (int)lv_slider_get_value(brightness_slider));
    // Update the value during dragging; apply brightness only when released.
    if (lv_event_get_target(event) == brightness_slider &&
        lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) return;
    webcam_status_t state;
    pipeline_snapshot(&state);
    const uint16_t delays[] = {0, 30, 60, 300};
    const unsigned rotation = lv_dropdown_get_selected(rotation_dropdown) * 90;
    state.settings.brightness = lv_slider_get_value(brightness_slider);
    state.settings.preview = lv_obj_has_state(preview_switch, LV_STATE_CHECKED);
    state.settings.mirror = lv_obj_has_state(mirror_switch, LV_STATE_CHECKED);
    state.settings.sleep_seconds = delays[lv_dropdown_get_selected(sleep_dropdown)];
    state.settings.rotation = rotation;
    pipeline_settings(&state.settings);
    if (!state.settings.preview) {
        preview_valid = false;
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int width, intptr_t action)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, 48);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x3b3c3d), LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_t *caption = lv_label_create(obj);
    lv_label_set_text(caption, text);
    lv_obj_center(caption);
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED, (void *)action);
    return caption;
}

static void style_switch(lv_obj_t *obj)
{
    lv_obj_set_size(obj, 64, 36);
    lv_obj_set_ext_click_area(obj, 6);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x3b3c3d), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_MUTED), LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_ACCENT), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_TEXT), LV_PART_KNOB);
}

static lv_obj_t *settings_dropdown(lv_obj_t *parent, int y, const char *choices)
{
    lv_obj_t *obj = lv_dropdown_create(parent);
    lv_obj_set_pos(obj, 270, y);
    lv_obj_set_size(obj, 190, 48);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_dropdown_set_options(obj, choices);
    lv_obj_t *options = lv_dropdown_get_list(obj);
    lv_obj_set_style_bg_color(options, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_text_color(options, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_color(options, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_border_width(options, 1, 0);
    lv_obj_set_style_text_line_space(options, 26, 0);
    lv_obj_set_style_bg_color(options, lv_color_hex(UI_ACCENT), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(options, lv_color_hex(UI_BACKGROUND), LV_PART_SELECTED | LV_STATE_CHECKED);
    return obj;
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    webcam_status_t state;
    pipeline_snapshot(&state);
    lv_indev_t *touch = webcam_display_get_input_dev();
    if (touch && lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED) last_activity = esp_timer_get_time();
    if (!state.screen_off && state.settings.sleep_seconds &&
        esp_timer_get_time() - last_activity > (int64_t)state.settings.sleep_seconds * 1000000) {
        pipeline_screen_off(true);
        state.screen_off = true;
        lv_obj_remove_flag(wake_cover, LV_OBJ_FLAG_HIDDEN);
    }
    int brightness = state.screen_off ? 0 : state.settings.brightness;
    if (brightness != brightness_applied) {
        esp_err_t err = bsp_display_brightness_set(brightness);
        if (err == ESP_OK) brightness_applied = brightness;
        else ESP_LOGW(TAG, "Brightness update: %s", esp_err_to_name(err));
    }
    if (state.screen_off) {
        show_camera_guide(false);
        return;
    }
    const bool standby = !state.settings.preview && !state.streaming;
    const bool camera_failed = state.error != ESP_OK && !standby;
    // Debounced slot presence distinguishes removal from camera initialization and capture failures.
    const bool needs_connection = !state.privacy && !standby &&
                                  camera_presence() == MOSAICO_MODULE_PRESENCE_ABSENT;
    if (state.privacy || standby) {
        guide_waiting_for_preview = false;
    } else if (needs_connection) {
        // A frame from the previous connection cannot satisfy the next connection's first-frame wait.
        preview_valid = false;
        guide_waiting_for_preview = true;
    } else if (!state.settings.preview) {
        guide_waiting_for_preview = false;
    }
    const bool guide_active = needs_connection || guide_waiting_for_preview;
    if (guide_active) {
        const char *title = needs_connection ? translated("将摄像头插入左侧 H2", "Insert the camera into left H2") :
                            translated("正在启动摄像头…", "Starting camera...");
        const char *hint = needs_connection ? translated("对准接口，轻推到位", "Align the connector, then push gently") :
                           camera_failed ? translated("正在自动重试", "Retrying automatically") : "";
        if (strcmp(lv_label_get_text(guide_title), title) != 0) lv_label_set_text(guide_title, title);
        if (strcmp(lv_label_get_text(guide_hint), hint) != 0) lv_label_set_text(guide_hint, hint);
    }
    const char *name = state.privacy ? translated("隐私保护已开启", "Privacy on") :
                       needs_connection ? translated("请连接摄像头", "Connect the camera") :
                       camera_failed ? translated("摄像头故障", "Camera error") :
                       standby ? translated("待机", "Standby") :
                       state.camera_ok ? translated("摄像头已开启", "Camera on") : translated("正在启动", "Starting");
    uint32_t color = state.privacy ? UI_TEXT : needs_connection ? UI_ACCENT : camera_failed ? UI_ERROR :
                     standby ? UI_MUTED : state.camera_ok ? UI_SUCCESS : UI_WARNING;
    lv_label_set_text(state_label, name);
    lv_obj_set_style_text_color(state_label, lv_color_hex(color), 0);
    lv_label_set_text(preview_value, state.settings.preview ? translated("开", "On") : translated("关", "Off"));
    lv_label_set_text(mirror_value, state.settings.mirror ? translated("开", "On") : translated("关", "Off"));
    const char *usb = !state.mounted ? translated("USB 未连接", "USB disconnected") :
                      state.streaming ? translated("USB 传输中", "USB streaming") : translated("USB 已连接", "USB connected");
    lv_label_set_text(mode_label, usb);
    lv_label_set_text(privacy_label, state.privacy ? translated("开启摄像头", "Camera On") : translated("关闭摄像头", "Camera Off"));
    lv_obj_t *primary = lv_obj_get_parent(privacy_label);
    lv_obj_set_style_bg_color(primary, lv_color_hex(state.privacy ? UI_ACCENT : UI_TEXT), 0);
    lv_obj_set_style_bg_color(primary, lv_color_hex(state.privacy ? 0xe64501 : 0xd6d6de), LV_STATE_PRESSED);
    const char *message = state.privacy ? translated("摄像头已关闭\n点击“开启摄像头”开始", "Camera is off\nTap Camera On to begin") :
                          guide_active ? "" :
                          camera_failed ? translated("摄像头不可用\n请检查左侧 H2 摄像头模块\n正在自动重试", "Camera unavailable\nCheck the left H2 camera module\nRetrying automatically") :
                          !state.settings.preview ? translated("本地预览已关闭\nUSB 视频输出不受影响", "Local preview is off\nUSB output is unaffected") :
                          !preview_valid ? translated("正在启动摄像头…", "Starting camera...") : "";
    if (strcmp(lv_label_get_text(notice), message) != 0) lv_label_set_text(notice, message);
    show_camera_guide(guide_active && lv_obj_has_flag(settings_panel, LV_OBJ_FLAG_HIDDEN));
    if (state.privacy || guide_active || state.error != ESP_OK || !state.settings.preview || !preview_valid) {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        if (guide_active) lv_obj_add_flag(camera_art, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(camera_art, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(camera_art, LV_OBJ_FLAG_HIDDEN);
    }
}

esp_err_t preview_ui_init(void)
{
    mailbox = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(mailbox, ESP_ERR_NO_MEM, TAG, "mailbox");
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    output = heap_caps_aligned_calloc(128, 1, PREVIEW_BYTES, caps);
    front = heap_caps_aligned_calloc(128, 1, PREVIEW_BYTES, caps);
    ESP_RETURN_ON_FALSE(output && front, ESP_ERR_NO_MEM, TAG, "preview buffers");
    const ppa_client_config_t cfg = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    ESP_RETURN_ON_ERROR(ppa_register_client(&cfg, &ppa), TAG, "preview PPA");
    ESP_RETURN_ON_ERROR(webcam_display_start(), TAG, "display");
    display_rotation = webcam_display_get_rotation();
    ESP_RETURN_ON_FALSE(webcam_display_lock(1000), ESP_ERR_TIMEOUT, TAG, "display lock");
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BACKGROUND), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(screen, &webcam_font_20, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    title_label = label(screen, "Webcam", 40, 10);
    state_label = label(screen, "Privacy on", 40, 31);
    lv_obj_set_style_text_font(state_label, &webcam_font_14, 0);
    mode_label = label(screen, "", 246, 16);
    lv_obj_set_width(mode_label, 194);
    lv_obj_set_style_text_font(mode_label, &webcam_font_14, 0);
    lv_obj_set_style_text_align(mode_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(UI_MUTED), 0);
    descriptor = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = PREVIEW_WIDTH, .h = PREVIEW_HEIGHT, .stride = PREVIEW_WIDTH * 2},
        .data_size = PREVIEW_BYTES, .data = front,
    };
    image = lv_image_create(screen);
    lv_image_set_src(image, &descriptor);
    lv_obj_set_pos(image, 0, 48);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    camera_art = lv_image_create(screen);
    lv_image_set_src(camera_art, &webcam_camera_icon);
    lv_obj_set_pos(camera_art, 181, 120);
    notice = label(screen, "Camera off", 0, 282);
    lv_obj_set_width(notice, 480);
    lv_obj_set_style_text_align(notice, LV_TEXT_ALIGN_CENTER, 0);
    camera_guide = lv_obj_create(screen);
    lv_obj_remove_style_all(camera_guide);
    lv_obj_set_pos(camera_guide, 0, 112);
    lv_obj_set_size(camera_guide, 480, 270);
    lv_obj_remove_flag(camera_guide, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(camera_guide, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *device = lv_image_create(camera_guide);
    lv_image_set_src(device, &webcam_camera_guide_device);
    lv_obj_set_pos(device, 120, 32);
    guide_arrow = lv_image_create(camera_guide);
    lv_image_set_src(guide_arrow, &webcam_camera_guide_arrow);
    lv_obj_set_pos(guide_arrow, 199, 8);
    guide_title = label(camera_guide, "", 0, 209);
    lv_obj_set_width(guide_title, 480);
    lv_obj_set_style_text_align(guide_title, LV_TEXT_ALIGN_CENTER, 0);
    guide_hint = label(camera_guide, "", 0, 241);
    lv_obj_set_width(guide_hint, 480);
    lv_obj_set_style_text_font(guide_hint, &webcam_font_14, 0);
    lv_obj_set_style_text_align(guide_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(guide_hint, lv_color_hex(0xc3c3ca), 0);
    privacy_label = button(screen, "Camera On", 40, 414, 128, 0);
    lv_obj_set_style_text_color(lv_obj_get_parent(privacy_label), lv_color_hex(UI_BACKGROUND), 0);
    settings_label = button(screen, "Settings", 176, 414, 128, 1);
    screen_off_label = button(screen, "Screen Off\nPause preview", 312, 414, 128, 3);
    lv_obj_set_style_text_font(screen_off_label, &webcam_font_14, 0);
    lv_obj_set_style_text_align(screen_off_label, LV_TEXT_ALIGN_CENTER, 0);

    settings_panel = lv_obj_create(screen);
    lv_obj_set_size(settings_panel, 480, 480);
    lv_obj_set_style_pad_all(settings_panel, 0, 0);
    lv_obj_set_style_radius(settings_panel, 0, 0);
    lv_obj_set_style_bg_color(settings_panel, lv_color_hex(UI_BACKGROUND), 0);
    lv_obj_set_style_text_color(settings_panel, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_width(settings_panel, 0, 0);
    lv_obj_remove_flag(settings_panel, LV_OBJ_FLAG_SCROLLABLE);
    settings_title = label(settings_panel, "Settings", 20, 24);
    static const char *language_map[] = {"中文", "EN", ""};
    lv_obj_t *language_track = lv_obj_create(settings_panel);
    lv_obj_set_pos(language_track, 328, 18);
    lv_obj_set_size(language_track, 132, 36);
    lv_obj_set_style_bg_color(language_track, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_border_width(language_track, 0, 0);
    lv_obj_set_style_radius(language_track, 12, 0);
    lv_obj_remove_flag(language_track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    language_buttons = lv_buttonmatrix_create(settings_panel);
    lv_obj_set_pos(language_buttons, 328, 12);
    lv_obj_set_size(language_buttons, 132, 48);
    lv_buttonmatrix_set_map(language_buttons, language_map);
    lv_buttonmatrix_set_button_ctrl_all(language_buttons, LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    lv_buttonmatrix_set_one_checked(language_buttons, true);
    lv_obj_set_style_pad_all(language_buttons, 0, 0);
    // Internal padding expands each button's hit area without enlarging its visible surface.
    lv_obj_set_style_pad_top(language_buttons, 6, 0);
    lv_obj_set_style_pad_bottom(language_buttons, 6, 0);
    lv_obj_set_style_pad_column(language_buttons, 0, 0);
    lv_obj_set_style_border_width(language_buttons, 0, 0);
    lv_obj_set_style_radius(language_buttons, 12, 0);
    lv_obj_set_style_bg_opa(language_buttons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(language_buttons, lv_color_hex(UI_SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(language_buttons, lv_color_hex(UI_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(language_buttons, lv_color_hex(UI_MUTED), LV_PART_ITEMS);
    lv_obj_set_style_text_color(language_buttons, lv_color_hex(UI_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(language_buttons, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(language_buttons, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(language_buttons, 12, LV_PART_ITEMS);
    lv_obj_add_event_cb(language_buttons, language_changed, LV_EVENT_VALUE_CHANGED, NULL);
    brightness_label = label(settings_panel, "Brightness", 20, 70);
    brightness_value = label(settings_panel, "", 380, 70);
    lv_obj_set_width(brightness_value, 80);
    lv_obj_set_style_text_align(brightness_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(brightness_value, lv_color_hex(UI_ACCENT), 0);
    brightness_slider = lv_slider_create(settings_panel);
    lv_obj_set_pos(brightness_slider, 24, 112);
    lv_obj_set_size(brightness_slider, 432, 12);
    lv_obj_set_ext_click_area(brightness_slider, 18);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(UI_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(UI_ACCENT), LV_PART_KNOB);
    lv_slider_set_range(brightness_slider, 10, 100);
    preview_label = label(settings_panel, "Local preview", 20, 154);
    preview_switch = lv_switch_create(settings_panel);
    lv_obj_set_pos(preview_switch, 388, 148);
    style_switch(preview_switch);
    preview_value = label(settings_panel, "", 328, 158);
    lv_obj_set_style_text_font(preview_value, &webcam_font_14, 0);
    preview_hint = label(settings_panel, "Off pauses preview; USB video continues", 20, 190);
    lv_obj_set_style_text_font(preview_hint, &webcam_font_14, 0);
    lv_obj_set_style_text_color(preview_hint, lv_color_hex(UI_MUTED), 0);
    mirror_label = label(settings_panel, "Preview mirror", 20, 218);
    mirror_switch = lv_switch_create(settings_panel);
    lv_obj_set_pos(mirror_switch, 388, 212);
    style_switch(mirror_switch);
    mirror_value = label(settings_panel, "", 328, 222);
    lv_obj_set_style_text_font(mirror_value, &webcam_font_14, 0);
    rotation_label = label(settings_panel, "USB video rotation", 20, 274);
    rotation_dropdown = settings_dropdown(settings_panel, 262, "0°\n90°\n180°\n270°");
    rotation_hint = label(settings_panel, "Local preview follows device orientation", 20, 314);
    lv_obj_set_style_text_font(rotation_hint, &webcam_font_14, 0);
    lv_obj_set_style_text_color(rotation_hint, lv_color_hex(UI_MUTED), 0);
    sleep_label = label(settings_panel, "Screen timeout", 20, 354);
    sleep_dropdown = settings_dropdown(settings_panel, 342, "Never\n30 seconds\n1 minute\n5 minutes");
    back_label = button(settings_panel, "Back", 20, 414, 440, 2);
    webcam_status_t state;
    pipeline_snapshot(&state);
    lv_slider_set_value(brightness_slider, state.settings.brightness, LV_ANIM_OFF);
    lv_label_set_text_fmt(brightness_value, "%u%%", state.settings.brightness);
    if (state.settings.preview) lv_obj_add_state(preview_switch, LV_STATE_CHECKED);
    if (state.settings.mirror) lv_obj_add_state(mirror_switch, LV_STATE_CHECKED);
    lv_dropdown_set_selected(rotation_dropdown, state.settings.rotation / 90);
    lv_dropdown_set_selected(sleep_dropdown, state.settings.sleep_seconds == 0 ? 0 :
                             state.settings.sleep_seconds == 30 ? 1 : state.settings.sleep_seconds == 60 ? 2 : 3);
    lv_obj_add_event_cb(brightness_slider, settings_changed, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(brightness_slider, settings_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(preview_switch, settings_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(mirror_switch, settings_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sleep_dropdown, settings_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(rotation_dropdown, settings_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
    wake_cover = lv_obj_create(screen);
    lv_obj_set_size(wake_cover, 480, 480);
    lv_obj_set_pos(wake_cover, 0, 0);
    lv_obj_set_style_bg_color(wake_cover, lv_color_black(), 0);
    lv_obj_set_style_border_width(wake_cover, 0, 0);
    lv_obj_set_style_radius(wake_cover, 0, 0);
    lv_obj_remove_flag(wake_cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wake_cover, clicked, LV_EVENT_CLICKED, (void *)4);
    lv_obj_add_flag(wake_cover, LV_OBJ_FLAG_HIDDEN);
    apply_language(state.settings.language);
    last_activity = esp_timer_get_time();
    refresh(NULL);
    lv_timer_create(refresh, 250, NULL);
    webcam_display_unlock();
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(preview_task, "preview", 4096, NULL, 2, &preview_task_handle, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "preview task");
    if (xTaskCreate(orientation_task, "orientation", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Orientation task was not created; keeping the current display orientation");
    }
    return ESP_OK;
}
