/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/nvs.h — Nvs port (L0 persistence side).
 *
 * Key/value persistence for Wi-Fi + LLM config, calibration, and tuning. The
 * adapter wraps ESP-IDF nvs_flash; the core sees only get/set of str/i32. This
 * keeps the composition root's config seam host-testable with an in-memory
 * fake.
 */
#ifndef JR_PORTS_NVS_H
#define JR_PORTS_NVS_H

#include "jr_ports/jr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jr_nvs {
    void *ctx;
    jr_err_t (*get_str)(void *ctx, const char *key, char *out, size_t out_len);
    jr_err_t (*set_str)(void *ctx, const char *key, const char *val);
    jr_err_t (*get_i32)(void *ctx, const char *key, int32_t *out);
    jr_err_t (*set_i32)(void *ctx, const char *key, int32_t val);
} jr_nvs_t;

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_NVS_H */
