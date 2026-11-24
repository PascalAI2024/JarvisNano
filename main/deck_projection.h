/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DECK_PROJECTION_H
#define DECK_PROJECTION_H

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#define DECK_PROJECTION_MAX_WORKERS 4u
#define DECK_PROJECTION_MAX_LIVE 256u
#define DECK_PROJECTION_MAX_OWNER_COUNT 512u

typedef enum {
    DECK_WORKER_NONE = 0,
    DECK_WORKER_WORKING,
    DECK_WORKER_STARTING,
    DECK_WORKER_NEEDS_OWNER,
    DECK_WORKER_STALLED
} deck_worker_state_t;

/* Only counts and enums survive parsing. No source strings or pointers do. */
typedef struct {
    uint16_t live;
    uint16_t running;
    uint16_t remaining;
    uint16_t owner_count;
    uint8_t worker_count;
    bool needs_owner;
    bool fixture;
    deck_worker_state_t workers[DECK_PROJECTION_MAX_WORKERS];
} deck_projection_t;

/* Parse the bridge's exact seven-key deck object. Failure leaves out intact.
 * The caller must reject embedded NULs in raw JSON before cJSON decoding:
 * cJSON strings retain no length with which to distinguish a NUL suffix.
 */
bool deck_projection_parse(const cJSON *object, deck_projection_t *out);

#endif
