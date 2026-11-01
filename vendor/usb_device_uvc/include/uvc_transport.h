// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Check whether a TinyUSB video stream retains a frame buffer.
 * @note Call only from the TinyUSB task, serialized with video class callbacks.
 * @param[in] ctl_idx Video control interface index.
 * @param[in] stm_idx Streaming interface index within the control interface.
 * @return True if the stream retains a frame buffer; false if idle or the stream does not exist.
 */
bool uvc_transport_busy(uint_fast8_t ctl_idx, uint_fast8_t stm_idx);

/**
 * @brief Cancel a pending video transfer and clear the class driver's frame state.
 * @note Call only from the TinyUSB task, serialized with video class callbacks.
 * @param[in] ctl_idx Video control interface index; an unknown stream is ignored.
 * @param[in] stm_idx Streaming interface index within the control interface.
 */
void uvc_transport_abort(uint_fast8_t ctl_idx, uint_fast8_t stm_idx);
