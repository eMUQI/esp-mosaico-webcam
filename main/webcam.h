#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb_device_uvc.h"
#include "sdkconfig.h"
#include "webcam_settings.h"
#ifndef CONFIG_WEBCAM_STATIC_FRAME
#define CONFIG_WEBCAM_STATIC_FRAME 0
#endif
#ifndef CONFIG_WEBCAM_DIAGNOSTICS
#define CONFIG_WEBCAM_DIAGNOSTICS 0
#endif
#ifndef CONFIG_WEBCAM_SENSOR_MIRROR_Y
#define CONFIG_WEBCAM_SENSOR_MIRROR_Y 0
#endif
#ifndef CONFIG_WEBCAM_SENSOR_MIRROR_X
#define CONFIG_WEBCAM_SENSOR_MIRROR_X 0
#endif

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define FRAME_FPS CONFIG_UVC_CAM1_FRAMERATE
#define RAW_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 2U)
#define JPEG_CAPACITY RAW_BYTES
#define PREVIEW_WIDTH 480
#define PREVIEW_HEIGHT 360
#define PREVIEW_BYTES (PREVIEW_WIDTH * PREVIEW_HEIGHT * 2U)

/** @brief Borrowed RGB565 frame in a FRAME_WIDTH by FRAME_HEIGHT canvas. */
typedef struct {
    const void *data;                  /*!< Canvas pixels owned by the capture pipeline. */
    uint32_t epoch;                    /*!< Publication generation sampled before capture. */
    uint16_t x, y, width, height;       /*!< Active image rectangle in canvas pixels, excluding black bars. */
    uint16_t video_rotation;           /*!< Rotation already applied to the canvas, in degrees. */
} webcam_preview_frame_t;

/** @brief Pipeline state, cumulative counters, and most recent frame measurements. */
typedef struct {
    bool privacy;                     /*!< Suppress camera images and supply black USB frames. */
    bool screen_off;                  /*!< Request zero brightness and suspend local preview. */
    bool camera_ok;                   /*!< Most recent camera processing cycle succeeded. */
    bool streaming;                   /*!< Host has an active video stream and USB is not suspended. */
    bool mounted;                     /*!< USB device is configured by the host. */
    bool high_speed;                  /*!< USB link uses high-speed mode. */
    uint32_t epoch;                   /*!< Generation used to reject stale frames after session or image changes. */
    uint32_t captured;                /*!< Camera frames acquired, including warm-up frames. */
    uint32_t encoded;                 /*!< JPEG frames encoded and published to a slot. */
    uint32_t fed;                     /*!< USB frame requests served, including repeated and black frames. */
    uint32_t fresh;                   /*!< USB requests served with a new camera frame sequence. */
    uint32_t repeated;                /*!< USB requests served with the previous camera frame sequence. */
    uint32_t black_frames;            /*!< USB requests served with the fallback black frame. */
    uint32_t previewed;               /*!< Frames submitted to LVGL for local display. */
    uint32_t replaced;                /*!< Ready JPEG slots overwritten before reuse; not host packet loss. */
    uint32_t capture_errors;          /*!< Camera acquisition failures other than incomplete frames. */
    uint32_t incomplete;              /*!< Camera acquisition failures reported as ESP_ERR_INVALID_SIZE. */
    uint32_t restarts;                /*!< Camera restart attempts after acquisition timeouts. */
    uint32_t encode_errors;           /*!< JPEG encoding failures. */
    uint32_t preview_drops;           /*!< Frames skipped or rejected by the preview path. */
    uint32_t jpeg_bytes;              /*!< Most recently published JPEG size in bytes. */
    uint32_t encode_us;               /*!< Duration of the most recent JPEG encoding attempt, in microseconds. */
    uint32_t normalize_us;            /*!< Duration of the most recent normalization attempt, in microseconds. */
    uint32_t preview_us;              /*!< Processing time of the latest preview submitted to LVGL, in microseconds. */
    uint32_t frame_age_us;            /*!< Age of the most recent camera frame supplied to USB, in microseconds. */
    uint32_t sensor_fps;              /*!< Frame rate reported by the sensor configuration. */
    uint8_t outstanding;              /*!< Number of camera DMA frames currently held by the pipeline. */
    esp_err_t error;                  /*!< Most recent camera or encoding error; ESP_OK after successful processing. */
    webcam_settings_t settings;       /*!< Current settings, which may still await persistence to NVS. */
} webcam_status_t;

/**
 * @brief Initialize pipeline resources and load saved settings with privacy enabled.
 *
 * Call once before using other pipeline functions. Partial initialization is not rolled back on failure.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM on allocation failure, ESP_ERR_INVALID_ARG for invalid rotation,
 *         or an error propagated from the JPEG encoder, cache synchronization, or PPA driver.
 */
esp_err_t pipeline_init(void);

/**
 * @brief Copy pipeline state, then sample the current USB connection and stream state.
 * @param[out] status Non-NULL destination; USB fields are sampled after releasing the pipeline lock.
 */
void pipeline_snapshot(webcam_status_t *status);

/**
 * @brief Set privacy and invalidate queued or in-progress frames from the previous generation.
 * @param[in] enabled True to suppress camera images. Frames already handed to USB cannot be withdrawn.
 */
void pipeline_privacy(bool enabled);

/**
 * @brief Request display sleep or wake without interrupting an active USB video stream.
 * @param[in] off True to turn off the screen and pause local preview; false to resume display operation.
 */
void pipeline_screen_off(bool off);

/**
 * @brief Apply settings and schedule NVS persistence on the maintenance task.
 * @param[in] settings Non-NULL settings copied by value. Rotations other than 0, 90, 180, or 270 are ignored.
 */
void pipeline_settings(const webcam_settings_t *settings);

/**
 * @brief Record a displayed or dropped preview frame.
 * @param[in] rendered True if the frame was submitted to LVGL; false if it was skipped.
 * @param[in] elapsed_us Preview processing duration in microseconds; ignored for skipped frames.
 */
void pipeline_preview_result(bool rendered, uint32_t elapsed_us);

/**
 * @brief Return the currently borrowed preview canvas to the capture pipeline.
 * @param[in] rgb565 Exact pointer reserved for preview. Return it once, after all reads have completed.
 */
void pipeline_preview_release(const void *rgb565);

/**
 * @brief Start capture and maintenance tasks after pipeline and preview initialization.
 * @return ESP_OK on success, or ESP_ERR_NO_MEM if task creation fails. Previously started tasks remain active.
 */
esp_err_t pipeline_start_tasks(void);

/**
 * @brief Validate a UVC stream request and invalidate frames from the previous session.
 * @param[in] format Requested format; only UVC_FORMAT_JPEG is supported.
 * @param[in] width Requested width; must equal FRAME_WIDTH.
 * @param[in] height Requested height; must equal FRAME_HEIGHT.
 * @param[in] rate Requested frame rate; must equal FRAME_FPS.
 * @param[in] ctx Unused callback context; may be NULL.
 * @return ESP_OK for the configured mode, or ESP_ERR_NOT_SUPPORTED for any other mode.
 */
esp_err_t pipeline_uvc_start(uvc_format_t format, int width, int height, int rate, void *ctx);

/**
 * @brief Invalidate queued and in-progress frames when a UVC session ends.
 * @param[in] ctx Unused callback context; may be NULL.
 */
void pipeline_uvc_stop(void *ctx);

/**
 * @brief Borrow the latest eligible JPEG, or the black fallback when no camera frame is eligible.
 * @param[in] ctx Unused callback context; may be NULL.
 * @return Pipeline-owned frame. The serialized USB sender must return it through pipeline_uvc_return().
 */
uvc_fb_t *pipeline_uvc_get(void *ctx);

/**
 * @brief Return a borrowed USB frame after the sender has finished reading its contents.
 * @param[in] frame Frame obtained from pipeline_uvc_get(); the fallback black frame requires no slot release.
 * @param[in] ctx Unused callback context; may be NULL.
 */
void pipeline_uvc_return(uvc_fb_t *frame, void *ctx);

/**
 * @brief Initialize the display, preview resources, and preview and orientation tasks.
 *
 * Requires pipeline_init(). Orientation task failure is logged and does not prevent preview operation.
 * Partial initialization is not rolled back on failure.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM on allocation or preview task failure, ESP_FAIL on display startup
 *         failure, ESP_ERR_TIMEOUT on display lock timeout, or an error propagated from the PPA driver.
 */
esp_err_t preview_ui_init(void);

/**
 * @brief Submit one borrowed RGB565 canvas to the preview task without waiting for the mailbox.
 * @param[in] frame Frame metadata copied on acceptance; pixel data are not copied and must remain unchanged.
 * @return True if accepted; the preview task calls pipeline_preview_release() after its final read.
 *         False for invalid geometry or an unavailable mailbox; the caller retains release responsibility.
 */
bool preview_ui_submit(const webcam_preview_frame_t *frame);
