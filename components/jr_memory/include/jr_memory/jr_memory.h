/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_memory — small on-device fact store for remember/recall tool calls.
 *
 * NVS must be initialized before jr_memory_init().  Init is idempotent; all
 * operations after init are serialized and bounded.  Fact content is never
 * logged by this component.
 */
#ifndef JR_MEMORY_H
#define JR_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jr_memory/jr_memory_guard.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JR_MEMORY_MAX_ENTRIES       12U
#define JR_MEMORY_QUERY_MAX         96U
#define JR_MEMORY_RECALL_JSON_CAP   512U
#define JR_MEMORY_LIST_JSON_CAP     6144U

typedef struct {
    bool ready;
    uint16_t entries;
    uint16_t capacity;
    uint32_t generation;
    uint32_t successful_writes;
    uint32_t rejected_secrets;
    esp_err_t last_error;
} jr_memory_status_t;

/* Loads the newest valid NVS snapshot.  A missing namespace is a valid empty
 * store; corrupt/torn snapshots are ignored in favour of the other slot. */
esp_err_t jr_memory_init(void);

/* Insert or replace a fact by case-insensitive key.  The store never evicts a
 * fact silently: a new key returns ESP_ERR_NO_MEM when capacity is exhausted.
 * Validation details are available through jr_memory_validate_fact(). */
esp_err_t jr_memory_store(const char *key, const char *value);

/* Returns one JSON object.  Exact key match wins, then key substring, then
 * value substring (all ASCII case-insensitive).  A miss is {"found":false}.
 * Output is all-or-nothing: insufficient capacity returns
 * ESP_ERR_INVALID_SIZE and leaves an empty string. */
esp_err_t jr_memory_recall(const char *query, char *out, size_t out_cap,
                           bool *found);

/* Returns a JSON array of {"key","value"} objects.  No truncation. */
esp_err_t jr_memory_list(char *out, size_t out_cap, size_t *count);

/* Metadata only.  No fact content is exposed through status. */
esp_err_t jr_memory_status(jr_memory_status_t *out);

/* Persists an empty newer snapshot before retiring the older slot, so a power
 * loss cannot resurrect cleared facts.  This also recovers a store whose init
 * failed because neither snapshot was readable. */
esp_err_t jr_memory_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* JR_MEMORY_H */
