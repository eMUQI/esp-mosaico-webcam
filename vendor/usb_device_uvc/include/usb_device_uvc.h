/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <sys/time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UVC format
 */
typedef enum {
    UVC_FORMAT_JPEG,            /*!< JPEG format */
    UVC_FORMAT_H264,            /*!< H264 format */
} uvc_format_t;

/**
 * @brief Frame buffer structure
 */
typedef struct {
    uint8_t *buf;               /*!< Pointer to the frame data */
    size_t len;                 /*!< Length of the buffer in bytes */
    size_t width;               /*!< Width of the image frame in pixels */
    size_t height;              /*!< Height of the image frame in pixels */
    uvc_format_t format;        /*!< Format of the frame data */
    struct timeval timestamp;   /*!< Timestamp since boot of the frame */
} uvc_fb_t;

/**
 * @brief Callback invoked when the host commits a video stream configuration.
 * @param[in] format Requested video format.
 * @param[in] width Requested frame width in pixels.
 * @param[in] height Requested frame height in pixels.
 * @param[in] rate Requested frame rate in frames per second.
 * @param[in] cb_ctx Application context supplied in uvc_device_config_t.
 * @return ESP_OK to accept the configuration, or an error to reject it.
 */
typedef esp_err_t (*uvc_input_start_cb_t)(uvc_format_t format, int width, int height, int rate, void *cb_ctx);

/**
 * @brief Callback invoked when the USB sender requests a frame.
 * @param[in] cb_ctx Application context supplied in uvc_device_config_t.
 * @return Borrowed frame valid until the return callback, or NULL to skip this interval.
 */
typedef uvc_fb_t* (*uvc_input_fb_get_cb_t)(void *cb_ctx);

/**
 * @brief Callback invoked when the sender releases a borrowed frame after copying or rejecting it.
 * @param[in] fb Frame previously obtained through the frame acquisition callback.
 * @param[in] cb_ctx Application context supplied in uvc_device_config_t.
 */
typedef void (*uvc_input_fb_return_cb_t)(uvc_fb_t *fb, void *cb_ctx);

/**
 * @brief Callback invoked when a stream is stopped, suspended, or reconfigured.
 * @param[in] cb_ctx Application context supplied in uvc_device_config_t.
 */
typedef void (*uvc_input_stop_cb_t)(void *cb_ctx);

/**
 * @brief Configuration for the UVC device
 */
typedef struct {
    uint8_t *uvc_buffer;                   /*!< UVC transfer buffer */
    uint32_t uvc_buffer_size;              /*!< Transfer capacity in bytes; must accommodate each submitted frame. */
    uvc_input_start_cb_t start_cb;         /*!< Required callback for accepting a stream configuration. */
    uvc_input_fb_get_cb_t fb_get_cb;       /*!< Required callback for borrowing the next frame. */
    uvc_input_fb_return_cb_t fb_return_cb; /*!< Required callback for returning a borrowed frame. */
    uvc_input_stop_cb_t stop_cb;           /*!< Required callback for stopping the current stream session. */
    void *cb_ctx;                         /*!< Application callback context; may be NULL. */
} uvc_device_config_t;

/**
 * @brief Store the buffer and callbacks for a UVC device before initialization.
 *
 * @param[in] index Device index: 0, or 1 when dual-camera support is enabled.
 * @param[in] config Non-NULL configuration copied by value; keep its transfer buffer valid until deinitialization.
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if the index or configuration is invalid
 */
esp_err_t uvc_device_config(int index, uvc_device_config_t *config);

/**
 * @brief Initialize the UVC device, after this function is called, the UVC device will be visible to the host
 *       and the host can open the UVC device with the specific format and resolution.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if a required UVC device has not been configured, or the device is already initialized
 *     - ESP_ERR_NO_MEM if event group or TinyUSB task creation fails
 *     - ESP_FAIL if TinyUSB initialization fails
 *     - Other errors propagated from USB PHY initialization
 */
esp_err_t uvc_device_init(void);

/**
 * @brief Deinitialize the UVC device
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if a required UVC device is not configured, or the event group is NULL
 *
 */
esp_err_t uvc_device_deinit(void);

#ifdef __cplusplus
}
#endif
