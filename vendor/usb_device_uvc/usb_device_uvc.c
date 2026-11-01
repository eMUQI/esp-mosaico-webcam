/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_check.h"
#if CONFIG_TINYUSB_RHPORT_HS
#include "soc/hp_sys_clkrst_reg.h"
#include "soc/hp_system_reg.h"
#endif
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usb_device_uvc.h"
#include "usb_descriptors.h"
#include "uvc_transport.h"

static const char *TAG = "usbd_uvc";

#if CONFIG_UVC_SUPPORT_TWO_CAM
#define UVC_CAM_NUM 2
#else
#define UVC_CAM_NUM 1
#endif

#define TUSB_EVENT_EXIT         (1<<0)
#define TUSB_EVENT_EXIT_DONE    (1<<1)
typedef struct {
    usb_phy_handle_t phy_hdl;
    bool uvc_init[UVC_CAM_NUM];
    uvc_format_t format[UVC_CAM_NUM];
    uvc_device_config_t user_config[UVC_CAM_NUM];
    TaskHandle_t tusb_task_hdl;
    uint32_t interval_ms[UVC_CAM_NUM];
    EventGroupHandle_t event_group;
} uvc_device_t;

static uvc_device_t s_uvc_device;

static void video_service(unsigned index);

static esp_err_t usb_phy_init(void)
{
    // Configure USB PHY
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_PHY_TARGET_INT,
#if CONFIG_TINYUSB_RHPORT_HS
        .otg_speed = USB_PHY_SPEED_HIGH,
#endif
    };
    return usb_new_phy(&phy_conf, &s_uvc_device.phy_hdl);
}

static inline uint32_t get_time_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void tusb_device_task(void *arg)
{
    while (1) {
        EventBits_t uxBits = xEventGroupGetBits(s_uvc_device.event_group);
        if (uxBits & TUSB_EVENT_EXIT) {
            ESP_LOGI(TAG, "TUSB task exit");
            break;
        }
        // Serialize control requests, frame submission and recovery on the USB task.
        tud_task_ext(1, false);
        for (unsigned i = 0; i < UVC_CAM_NUM; ++i) video_service(i);
    }
    xEventGroupSetBits(s_uvc_device.event_group, TUSB_EVENT_EXIT_DONE);
    vTaskDelete(NULL);
}

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "Mount");
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "UN-Mount");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;

    if (s_uvc_device.user_config[0].stop_cb) {
        s_uvc_device.user_config[0].stop_cb(s_uvc_device.user_config[0].cb_ctx);
    }
#if CONFIG_UVC_SUPPORT_TWO_CAM
    if (s_uvc_device.user_config[1].stop_cb) {
        s_uvc_device.user_config[1].stop_cb(s_uvc_device.user_config[1].cb_ctx);
    }
#endif
    ESP_LOGI(TAG, "Suspend");
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "Resume");
}

#if (CFG_TUD_VIDEO)
//--------------------------------------------------------------------+
// USB Video
//--------------------------------------------------------------------+
static void video_service(unsigned index)
{
    static uint32_t submitted_ms[UVC_CAM_NUM];
    static bool active[UVC_CAM_NUM];
    const uint32_t now = get_time_millis();
    if (!tud_ready() || !tud_video_n_streaming(index, 0)) {
        active[index] = false;
        return;
    }
    if (!active[index]) {
        active[index] = true;
        submitted_ms[index] = now;
    }
    if (uvc_transport_busy(index, 0)) {
        if (now - submitted_ms[index] >= 1000) {
            ESP_LOGW(TAG, "Stream %u transfer timeout; cancelling pending frame", index);
            uvc_transport_abort(index, 0);
            submitted_ms[index] = now;
        }
        return;
    }
    if (now - submitted_ms[index] < s_uvc_device.interval_ms[index]) return;
    submitted_ms[index] = now;
    uvc_device_config_t *config = &s_uvc_device.user_config[index];
    uvc_fb_t *pic = config->fb_get_cb(config->cb_ctx);
    if (!pic) return;
    const size_t length = pic->len;
    if (!length || length > config->uvc_buffer_size) {
        ESP_LOGW(TAG, "Invalid frame size: %zu", length);
        config->fb_return_cb(pic, config->cb_ctx);
        return;
    }
    memcpy(config->uvc_buffer, pic->buf, length);
    config->fb_return_cb(pic, config->cb_ctx);
    if (!tud_video_n_frame_xfer(index, 0, config->uvc_buffer, length)) {
        ESP_LOGW(TAG, "Stream %u submission failed; retrying at next frame interval", index);
    }
}

int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx,
                        video_probe_and_commit_control_t const *parameters)
{
    // This application exposes one MJPEG mode with one fixed frame interval.
    if (!parameters || ctl_idx != 0 || stm_idx != 0 ||
        parameters->bFormatIndex != 1 || parameters->bFrameIndex != 1 ||
        parameters->dwFrameInterval != 10000000U / UVC_CAM1_FRAME_RATE) {
        return VIDEO_ERROR_OUT_OF_RANGE;
    }
    ESP_LOGI(TAG, "bFrameIndex: %u", parameters->bFrameIndex);
    ESP_LOGI(TAG, "dwFrameInterval: %" PRIu32 "", parameters->dwFrameInterval);
    if (parameters->bFrameIndex > UVC_FRAME_NUM) {
        return VIDEO_ERROR_OUT_OF_RANGE;
    }
    s_uvc_device.interval_ms[ctl_idx] = parameters->dwFrameInterval / 10000;
    int frame_index = parameters->bFrameIndex - 1;
    if (s_uvc_device.user_config[ctl_idx].stop_cb) {
        s_uvc_device.user_config[ctl_idx].stop_cb(s_uvc_device.user_config[ctl_idx].cb_ctx);
    }
    // End the old transfer before the class driver installs the new commit.
    uvc_transport_abort(ctl_idx, stm_idx);
    esp_err_t ret = s_uvc_device.user_config[ctl_idx].start_cb(s_uvc_device.format[ctl_idx], UVC_FRAMES_INFO[ctl_idx][frame_index].width,
                                                               UVC_FRAMES_INFO[ctl_idx][frame_index].height, UVC_FRAMES_INFO[ctl_idx][frame_index].rate, s_uvc_device.user_config[ctl_idx].cb_ctx);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed");
        return VIDEO_ERROR_OUT_OF_RANGE;
    }
    return VIDEO_ERROR_NONE;
}
#endif

esp_err_t uvc_device_config(int index, uvc_device_config_t *config)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < UVC_CAM_NUM, ESP_ERR_INVALID_ARG, TAG, "index is invalid");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->start_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "start_cb is NULL");
    ESP_RETURN_ON_FALSE(config->fb_get_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "fb_get_cb is NULL");
    ESP_RETURN_ON_FALSE(config->fb_return_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "fb_return_cb is NULL");
    ESP_RETURN_ON_FALSE(config->stop_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "stop_cb is NULL");
    ESP_RETURN_ON_FALSE(config->uvc_buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "uvc_buffer is NULL");
    ESP_RETURN_ON_FALSE(config->uvc_buffer_size > 0, ESP_ERR_INVALID_ARG, TAG, "uvc_buffer_size is 0");

    s_uvc_device.user_config[index] = *config;
    s_uvc_device.interval_ms[index] = 1000 / (index == 0 ? UVC_CAM1_FRAME_RATE : UVC_CAM2_FRAME_RATE);
    s_uvc_device.uvc_init[index] = true;
    return ESP_OK;
}

esp_err_t uvc_device_init(void)
{
    ESP_RETURN_ON_FALSE(!s_uvc_device.event_group, ESP_ERR_INVALID_STATE, TAG, "UVC device already initialized");
    ESP_RETURN_ON_FALSE(s_uvc_device.uvc_init[0], ESP_ERR_INVALID_STATE, TAG, "uvc device 0 not init");
#if CONFIG_UVC_SUPPORT_TWO_CAM
    ESP_RETURN_ON_FALSE(s_uvc_device.uvc_init[1], ESP_ERR_INVALID_STATE, TAG, "uvc device 1 not init, if not use, please disable CONFIG_UVC_SUPPORT_TWO_CAM");
#endif

#if CONFIG_FORMAT_MJPEG_CAM1
    s_uvc_device.format[0] = UVC_FORMAT_JPEG;
#elif CONFIG_FORMAT_H264_CAM1
    s_uvc_device.format[0] = UVC_FORMAT_H264;
#endif

#if CONFIG_UVC_SUPPORT_TWO_CAM
#if CONFIG_FORMAT_MJPEG_CAM2
    s_uvc_device.format[1] = UVC_FORMAT_JPEG;
#elif CONFIG_FORMAT_H264_CAM2
    s_uvc_device.format[1] = UVC_FORMAT_H264;
#endif
#endif

    s_uvc_device.event_group = xEventGroupCreate();
    if (s_uvc_device.event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // init device stack on configured roothub port
    esp_err_t err = usb_phy_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB PHY initialization failed: %s", esp_err_to_name(err));
        goto fail;
    }
    bool usb_init = tusb_init();
    if (!usb_init) {
        ESP_LOGE(TAG, "USB Device Stack Init Fail");
        tusb_teardown();
        err = ESP_FAIL;
        goto fail;
    }

    BaseType_t core_id = (CONFIG_UVC_TINYUSB_TASK_CORE < 0) ? tskNO_AFFINITY : CONFIG_UVC_TINYUSB_TASK_CORE;
    if (xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 4096, NULL, CONFIG_UVC_TINYUSB_TASK_PRIORITY, &s_uvc_device.tusb_task_hdl, core_id) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TinyUSB task");
        tusb_teardown();
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "UVC Device Start, Version: %d.%d.%d", USB_DEVICE_UVC_VER_MAJOR, USB_DEVICE_UVC_VER_MINOR, USB_DEVICE_UVC_VER_PATCH);
    return ESP_OK;

fail:
    if (s_uvc_device.phy_hdl) {
        usb_del_phy(s_uvc_device.phy_hdl);
        s_uvc_device.phy_hdl = NULL;
    }
    vEventGroupDelete(s_uvc_device.event_group);
    s_uvc_device.event_group = NULL;
    s_uvc_device.tusb_task_hdl = NULL;
    return err;
}

esp_err_t uvc_device_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_uvc_device.uvc_init[0], ESP_ERR_INVALID_STATE, TAG, "uvc device 0 not init");
#if CONFIG_UVC_SUPPORT_TWO_CAM
    ESP_RETURN_ON_FALSE(s_uvc_device.uvc_init[1], ESP_ERR_INVALID_STATE, TAG, "uvc device 1 not init, if not use, please disable CONFIG_UVC_SUPPORT_TWO_CAM");
#endif
    ESP_RETURN_ON_FALSE(s_uvc_device.event_group != NULL, ESP_ERR_INVALID_STATE, TAG, "event group is NULL");

    // Stop TinyUSB task
    xEventGroupSetBits(s_uvc_device.event_group, TUSB_EVENT_EXIT);
    EventBits_t bits = xEventGroupWaitBits(s_uvc_device.event_group, TUSB_EVENT_EXIT_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(5000));
    if (!(bits & TUSB_EVENT_EXIT_DONE)) {
        ESP_LOGW(TAG, "TinyUSB task exit timeout (5s), force delete");
        if (s_uvc_device.tusb_task_hdl) {
            vTaskDelete(s_uvc_device.tusb_task_hdl);
            s_uvc_device.tusb_task_hdl = NULL;
        }
    }

    // Call user stop callbacks
    if (s_uvc_device.user_config[0].stop_cb) {
        s_uvc_device.user_config[0].stop_cb(s_uvc_device.user_config[0].cb_ctx);
    }
#if CONFIG_UVC_SUPPORT_TWO_CAM
    if (s_uvc_device.user_config[1].stop_cb) {
        s_uvc_device.user_config[1].stop_cb(s_uvc_device.user_config[1].cb_ctx);
    }
#endif

    // Clean up event group
    vEventGroupDelete(s_uvc_device.event_group);
    s_uvc_device.event_group = NULL;

    // Teardown USB stack
    tusb_teardown();
    if (s_uvc_device.phy_hdl) {
        usb_del_phy(s_uvc_device.phy_hdl);
        s_uvc_device.phy_hdl = NULL;
    }

    // Reset initialization flags
    memset(s_uvc_device.uvc_init, 0, sizeof(s_uvc_device.uvc_init));

    ESP_LOGI(TAG, "UVC Device Deinit");
    return ESP_OK;
}
