/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_tools_project — pure C on-device JSON budget projector.
 *
 * Fits an arbitrary JarvisMCP result cJSON tree into a fixed response slot
 * (typically JR_TOOLS_RESPONSE_CAP = 3072 B) without server changes and
 * without ESP-IDF dependencies. Compiles identically on host and ESP32-S3
 * (where cJSON allocations use PSRAM via cJSON_InitHooks).
 */
#ifndef JR_TOOLS_JR_TOOLS_PROJECT_H
#define JR_TOOLS_JR_TOOLS_PROJECT_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JR_TOOLS_PROJECT_HINT "trimmed on device; ask narrower if needed"

typedef struct {
    bool truncated;     /* true if noise stripping, tiering, or fallback ran */
    bool used_fallback; /* true if even the tightest structured tier overflowed */
    /* -1: fit untouched, 0: noise-strip only, 1..N: structured tier, N+1: fallback */
    int tier_index;
} jr_tools_project_info_t;

/* Serializes {"result": <upstream>} into out[0..out_cap-1].
 *
 * 1. If the untouched payload fits in out_cap, writes it verbatim with no
 *    heap duplication and sets info->truncated = false, tier_index = -1.
 * 2. Otherwise strips known bulky noise keys (raw, html, raw_html, embedding,
 *    embeddings, vector, debug, trace, stack, headers, markdown_full) and
 *    tries progressive structured compaction tiers, recording <key>_total for
 *    trimmed arrays and marking "truncated":true with JR_TOOLS_PROJECT_HINT.
 * 3. Only if even the tightest structured tier exceeds out_cap does it fall
 *    back to a UTF-8-safe escaped string-head envelope.
 *
 * Always guarantees valid JSON strictly shorter than out_cap on true return.
 * On false return (invalid args or tiny out_cap), out is empty if non-NULL. */
bool jr_tools_project_json(const cJSON *upstream, char *out, size_t out_cap,
                           jr_tools_project_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* JR_TOOLS_JR_TOOLS_PROJECT_H */
