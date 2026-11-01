#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WEBCAM_LANGUAGE_ZH = 0,
    WEBCAM_LANGUAGE_EN = 1,
} webcam_language_t;

#define WEBCAM_DEFAULT_SLEEP_SECONDS 300

/** @brief Persistent user settings shared by the pipeline and local display. */
typedef struct {
    uint8_t brightness;       /*!< Display brightness percentage, from 10 to 100. */
    bool preview;            /*!< Enable local preview independently of USB output. */
    bool mirror;             /*!< Mirror the local preview horizontally; USB output is unaffected. */
    uint16_t sleep_seconds;   /*!< Display idle timeout: 0 disables sleep; otherwise 30, 60, or 300 seconds. */
    uint16_t rotation;        /*!< USB canvas rotation in degrees: 0, 90, 180, or 270. */
    webcam_language_t language; /*!< Display language; USB device identity is unchanged. */
} webcam_settings_t;

/**
 * @brief Validate and decode a versioned NVS settings blob.
 *
 * Version 1 uses six bytes and retains the caller's rotation. Version 2 adds a seventh byte storing
 * rotation as a number of quarter turns. Version 3 adds the display language. Older versions use Chinese.
 * The sleep timeout is stored as a little-endian 16-bit value.
 *
 * @param[in,out] settings Non-NULL destination with a default rotation already set; unchanged on failure.
 * @param[in] values Non-NULL blob containing at least size bytes.
 * @param[in] size Blob size in bytes: 6 for version 1, 7 for version 2, or 8 for version 3.
 * @return True if all fields are valid and decoded; false for an unsupported version, size, or field value.
 */
static inline bool webcam_settings_decode(webcam_settings_t *settings, const uint8_t *values, size_t size)
{
    if (!((size == 6 && values[0] == 1) || (size == 7 && values[0] == 2) ||
          (size == 8 && values[0] == 3))) return false;
    if (values[1] < 10 || values[1] > 100 || values[2] > 1 || values[3] > 1) return false;
    unsigned sleep = values[4] | (values[5] << 8);
    if (sleep != 0 && sleep != 30 && sleep != 60 && sleep != 300) return false;
    if (size >= 7 && values[6] > 3) return false;
    if (size == 8 && values[7] > WEBCAM_LANGUAGE_EN) return false;
    settings->brightness = values[1];
    settings->preview = values[2];
    settings->mirror = values[3];
    settings->sleep_seconds = sleep;
    if (size >= 7) settings->rotation = values[6] * 90;
    settings->language = size == 8 ? (webcam_language_t)values[7] : WEBCAM_LANGUAGE_ZH;
    return true;
}
