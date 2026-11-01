#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Compute the preview rotation for the H2 camera's 270-degree mounting orientation.
 * @param[in] video_rotation Rotation already applied to the USB canvas, in quarter-turn degrees.
 * @param[in] panel_rotation Current BSP display rotation, in quarter-turn degrees.
 * @param[in] mirror_x Horizontal sensor mounting correction, before the additional USB reflection.
 * @param[in] mirror_y Vertical sensor mounting correction.
 * @return Additional PPA rotation in degrees, in the range 0 to 270.
 */
static inline unsigned preview_rotation(unsigned video_rotation, unsigned panel_rotation,
                                         bool mirror_x, bool mirror_y)
{
    unsigned correction = (270U + 360U - video_rotation) % 360U;
    // A single-axis reflection reverses the sign of a rotation.
    if (mirror_x != mirror_y) correction = (360U - correction) % 360U;
    return (correction + 360U - panel_rotation) % 360U;
}

/**
 * @brief Fit a rotated image inside a target canvas using PPA scale steps of one sixteenth.
 * @param[in] width Source width in pixels, from 1 to 4096.
 * @param[in] height Source height in pixels, from 1 to 4096.
 * @param[in] rotation Rotation in degrees: 0, 90, 180, or 270.
 * @param[in] target_width Target canvas width in pixels, from 1 to 4096.
 * @param[in] target_height Target canvas height in pixels, from 1 to 4096.
 * @param[out] scale16 Non-NULL destination for the scale factor multiplied by 16; valid only on success.
 * @param[out] x Non-NULL destination for the centered, even horizontal offset in pixels; valid only on success.
 * @param[out] y Non-NULL destination for the centered, even vertical offset in pixels; valid only on success.
 * @return True if the dimensions and rotation support a scale factor from 1/16 to 255/16; false otherwise.
 */
static inline bool frame_fit(uint32_t width, uint32_t height, unsigned rotation,
                             uint32_t target_width, uint32_t target_height,
                             unsigned *scale16, unsigned *x, unsigned *y)
{
    if (!width || !height || rotation % 90 || rotation > 270) return false;
    if (rotation == 90 || rotation == 270) {
        uint32_t temp = width;
        width = height;
        height = temp;
    }
    if (width > 4096 || height > 4096 || target_width > 4096 || target_height > 4096) return false;
    unsigned sx = target_width * 16U / width;
    unsigned sy = target_height * 16U / height;
    *scale16 = sx < sy ? sx : sy;
    if (!*scale16 || *scale16 > 255) return false;
    *x = ((target_width - width * *scale16 / 16U) / 2U) & ~1U;
    *y = ((target_height - height * *scale16 / 16U) / 2U) & ~1U;
    return true;
}

/**
 * @brief Check whether privacy and frame generation permit publication.
 * @param[in] privacy Whether camera images are currently suppressed.
 * @param[in] current_epoch Current pipeline generation.
 * @param[in] captured_epoch Generation sampled before this frame was captured.
 * @return True if privacy is disabled and the frame belongs to the current generation.
 */
static inline bool frame_publish_allowed(bool privacy, uint32_t current_epoch,
                                         uint32_t captured_epoch)
{
    return !privacy && current_epoch == captured_epoch;
}
