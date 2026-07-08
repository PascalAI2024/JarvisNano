/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/jr_types.h — shared primitive types for the port interfaces.
 *
 * THE ONE RULE (see gigabrain/architecture.md): nothing under jr_ports,
 * jr_core, or jr_dsp may include an ESP-IDF, driver, network, or model
 * header. These are the vocabulary the pure core speaks. Adapters translate
 * between these and esp_err_t / esp_codec_dev / cJSON at the edges.
 */
#ifndef JR_PORTS_JR_TYPES_H
#define JR_PORTS_JR_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provider-neutral result code. Adapters map esp_err_t <-> jr_err_t. */
typedef enum {
    JR_OK = 0,
    JR_ERR_FAIL = -1,       /* generic failure */
    JR_ERR_WOULD_BLOCK = -2, /* backpressure, NOT an error (see L2 framer) */
    JR_ERR_TIMEOUT = -3,
    JR_ERR_INVALID = -4,
    JR_ERR_NO_MEM = -5,
    JR_ERR_CLOSED = -6,
} jr_err_t;

/* Audio is always signed 16-bit PCM mono across every port. Sample rate is a
 * property of the lane, documented at the port, never carried in the frame. */
typedef int16_t jr_pcm_t;

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_JR_TYPES_H */
