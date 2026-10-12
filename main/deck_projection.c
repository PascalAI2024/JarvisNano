/* SPDX-License-Identifier: Apache-2.0 */
#include "deck_projection.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Walk each object once: cJSON lookup alone would conceal duplicate fields. */
static bool fields(const cJSON *object, const char *const *keys, size_t count,
                   const cJSON **values)
{
    if (!cJSON_IsObject(object)) return false;
    unsigned seen = 0;
    size_t visited = 0;
    for (const cJSON *item = object->child; item; item = item->next) {
        if (++visited > count || !item->string) return false;
        size_t key = 0;
        for (; key < count; ++key) {
            if (strcmp(item->string, keys[key]) == 0) break;
        }
        if (key == count || (seen & (1u << key))) return false;
        seen |= 1u << key;
        values[key] = item;
    }
    return visited == count;
}

static bool text_is(const cJSON *item, const char *expected)
{
    return cJSON_IsString(item) && item->valuestring &&
           strcmp(item->valuestring, expected) == 0;
}

static bool count_value(const cJSON *item, unsigned maximum, uint16_t *out)
{
    if (!cJSON_IsNumber(item)) return false;
    const double value = item->valuedouble;
    if (!isfinite(value) || value < 0 || value > maximum || floor(value) != value)
        return false;
    *out = (uint16_t)value;
    return true;
}

static bool worker_state(const cJSON *item, deck_worker_state_t *out)
{
    if (text_is(item, "working")) *out = DECK_WORKER_WORKING;
    else if (text_is(item, "starting")) *out = DECK_WORKER_STARTING;
    else if (text_is(item, "needs-you")) *out = DECK_WORKER_NEEDS_OWNER;
    else if (text_is(item, "stalled")) *out = DECK_WORKER_STALLED;
    else return false;
    return true;
}

bool deck_projection_parse(const cJSON *object, deck_projection_t *out)
{
    static const char *const keys[] = {
        "source", "workers", "live", "running", "remaining",
        "needs_owner", "owner_count"
    };
    static const char *const worker_keys[] = {"id", "label", "state"};
    static const char *const ids[] = {"w1", "w2", "w3", "w4"};
    static const char *const labels[] = {
        "WORKER 1", "WORKER 2", "WORKER 3", "WORKER 4"
    };
    const cJSON *value[7] = {0};
    deck_projection_t parsed = {0};
    if (!out || !fields(object, keys, 7, value)) return false;

    if (text_is(value[0], "fixture")) parsed.fixture = true;
    else if (!text_is(value[0], "live")) return false;
    if (!cJSON_IsArray(value[1]) ||
        !count_value(value[2], DECK_PROJECTION_MAX_LIVE, &parsed.live) ||
        !count_value(value[3], parsed.live, &parsed.running) ||
        !count_value(value[4], DECK_PROJECTION_MAX_LIVE, &parsed.remaining) ||
        !cJSON_IsBool(value[5]) ||
        !count_value(value[6], DECK_PROJECTION_MAX_OWNER_COUNT, &parsed.owner_count))
        return false;
    parsed.needs_owner = cJSON_IsTrue(value[5]);
    if (parsed.needs_owner != (parsed.owner_count > 0)) return false;

    const unsigned expected_workers = parsed.live < DECK_PROJECTION_MAX_WORKERS
        ? parsed.live : DECK_PROJECTION_MAX_WORKERS;
    const unsigned expected_remaining = parsed.live - expected_workers;
    if (parsed.remaining != expected_remaining) return false;

    unsigned shown_running = 0;
    for (const cJSON *worker = value[1]->child; worker; worker = worker->next) {
        const cJSON *part[3] = {0};
        const unsigned index = parsed.worker_count;
        if (index >= expected_workers || index >= DECK_PROJECTION_MAX_WORKERS ||
            !fields(worker, worker_keys, 3, part) ||
            !text_is(part[0], ids[index]) || !text_is(part[1], labels[index]) ||
            !worker_state(part[2], &parsed.workers[index]))
            return false;
        shown_running += parsed.workers[index] == DECK_WORKER_WORKING;
        if (parsed.workers[index] == DECK_WORKER_NEEDS_OWNER && !parsed.needs_owner)
            return false;
        ++parsed.worker_count;
    }
    if (parsed.worker_count != expected_workers ||
        parsed.running < shown_running ||
        parsed.running > shown_running + parsed.remaining)
        return false;

    *out = parsed;
    return true;
}
