// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/** @brief Zero-initialized orientation debounce state with a retained confirmed target. */
typedef struct {
    unsigned candidate;  /*!< Rotation indicated by the current run of eligible samples, in degrees. */
    unsigned desired;    /*!< Most recently confirmed target rotation, in degrees. */
    uint8_t samples;     /*!< Consecutive eligible samples for the candidate, capped at three. */
    bool confirmed;      /*!< At least one target has been confirmed since initialization. */
} orientation_policy_t;

/**
 * @brief Confirm display orientation after three consecutive eligible acceleration samples.
 *
 * Dominant +Y, +X, -Y, and -X map to 0, 90, 180, and 270 degrees respectively. An eligible sample has
 * a dominant magnitude of at least 0.65 g and at least 1.15 times the other axis magnitude.
 * A confirmed target remains available on subsequent calls until it matches the applied rotation.
 *
 * @param[in,out] state Non-NULL debounce state; initialize to zero before the first sample.
 * @param[in] ax Acceleration along the board X axis, in g.
 * @param[in] ay Acceleration along the board Y axis, in g.
 * @param[in] applied Current display rotation in degrees.
 * @param[out] requested Non-NULL destination for a confirmed target; written only when returning true.
 * @return True if a confirmed target differs from the applied rotation; false otherwise.
 */
static inline bool orientation_update(orientation_policy_t *state, float ax, float ay,
                                      unsigned applied, unsigned *requested)
{
    const float x = fabsf(ax), y = fabsf(ay);
    const float dominant = x >= y ? x : y;
    const float other = x >= y ? y : x;
    if (!isfinite(ax) || !isfinite(ay) || dominant < 0.65f || dominant < other * 1.15f) {
        state->samples = 0;
    } else {
        const unsigned candidate = x >= y ? (ax >= 0 ? 90 : 270) : (ay >= 0 ? 0 : 180);
        if (!state->samples || candidate != state->candidate) {
            state->candidate = candidate;
            state->samples = 1;
        } else if (state->samples < 3) {
            ++state->samples;
        }
        if (state->samples == 3) {
            state->desired = candidate;
            state->confirmed = true;
        }
    }
    if (!state->confirmed || state->desired == applied) return false;
    *requested = state->desired;
    return true;
}
