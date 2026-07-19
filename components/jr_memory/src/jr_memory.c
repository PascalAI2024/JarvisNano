/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dual-snapshot NVS fact store.  Mutations write and commit the inactive slot
 * before replacing RAM state.  On boot, the newest CRC-valid generation wins;
 * a torn write therefore falls back to the previous complete snapshot.
 */
#include "jr_memory/jr_memory.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"

#define JR_MEMORY_NAMESPACE       "jr_memory"
#define JR_MEMORY_SLOT_0          "slot0"
#define JR_MEMORY_SLOT_1          "slot1"
#define JR_MEMORY_MAGIC           UINT32_C(0x4A524D35) /* JRM5 */
#define JR_MEMORY_FORMAT_VERSION  1U
#define JR_MEMORY_LOCK_TIMEOUT_MS 2000U

typedef struct {
    uint8_t used;
    uint8_t reserved0[3];
    char key[JR_MEMORY_KEY_MAX + 1U];
    char value[JR_MEMORY_VALUE_MAX + 1U];
    uint8_t reserved1[2];
} jr_memory_entry_disk_t;

typedef struct {
    uint32_t magic;
    uint32_t generation;
    uint16_t version;
    uint16_t count;
    uint32_t reserved;
    jr_memory_entry_disk_t entries[JR_MEMORY_MAX_ENTRIES];
    uint32_t crc32;
} jr_memory_snapshot_t;

_Static_assert(sizeof(jr_memory_entry_disk_t) % 4U == 0U,
               "memory entry must preserve snapshot alignment");
_Static_assert(sizeof(jr_memory_snapshot_t) <= 3072U,
               "memory snapshot must remain small enough for the NVS partition");

typedef struct {
    char *out;
    size_t cap;
    size_t used;
    bool failed;
} json_writer_t;

static const char *TAG = "jr_memory";
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static atomic_flag s_lock_init_guard = ATOMIC_FLAG_INIT;
static jr_memory_snapshot_t s_snapshot;
/* Shared mutation/load scratch. Operations hold s_lock, keeping 2.8 KiB
 * snapshots off the comparatively small caller task stacks. */
static jr_memory_snapshot_t s_work;
static int s_active_slot = -1;
static bool s_ready;
static _Atomic uint32_t s_successful_writes;
static _Atomic uint32_t s_rejected_secrets;
static _Atomic int s_last_error;

static uint32_t crc32_ieee(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < len; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static uint32_t snapshot_crc(const jr_memory_snapshot_t *snapshot)
{
    return crc32_ieee(snapshot, offsetof(jr_memory_snapshot_t, crc32));
}

static void snapshot_blank(jr_memory_snapshot_t *snapshot, uint32_t generation)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = JR_MEMORY_MAGIC;
    snapshot->generation = generation == 0U ? 1U : generation;
    snapshot->version = JR_MEMORY_FORMAT_VERSION;
    snapshot->crc32 = snapshot_crc(snapshot);
}

static bool terminated_within(const char *value, size_t cap)
{
    return memchr(value, '\0', cap) != NULL;
}

static bool snapshot_valid(const jr_memory_snapshot_t *snapshot)
{
    if (snapshot->magic != JR_MEMORY_MAGIC ||
        snapshot->version != JR_MEMORY_FORMAT_VERSION ||
        snapshot->generation == 0U ||
        snapshot->count > JR_MEMORY_MAX_ENTRIES ||
        snapshot->crc32 != snapshot_crc(snapshot)) {
        return false;
    }

    uint16_t used = 0U;
    for (size_t i = 0U; i < JR_MEMORY_MAX_ENTRIES; ++i) {
        const jr_memory_entry_disk_t *entry = &snapshot->entries[i];
        if (entry->used == 0U) {
            continue;
        }
        if (entry->used != 1U ||
            !terminated_within(entry->key, sizeof(entry->key)) ||
            !terminated_within(entry->value, sizeof(entry->value)) ||
            jr_memory_validate_fact(entry->key, entry->value) != JR_MEMORY_VALID) {
            return false;
        }
        ++used;
    }
    return used == snapshot->count;
}

static bool generation_newer(uint32_t candidate, uint32_t current)
{
    const uint32_t delta = candidate - current;
    return delta != 0U && delta < UINT32_C(0x80000000);
}

static SemaphoreHandle_t ensure_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_lock_init_guard,
                                              memory_order_acquire)) {
        /* A delay, rather than a same-priority yield, avoids priority
         * inversion if the creator is preempted by a higher-priority caller. */
        vTaskDelay(1U);
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    atomic_flag_clear_explicit(&s_lock_init_guard, memory_order_release);
    return s_lock;
}

static esp_err_t lock_store(void)
{
    SemaphoreHandle_t lock = ensure_lock();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return xSemaphoreTake(lock, pdMS_TO_TICKS(JR_MEMORY_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void unlock_store(void)
{
    xSemaphoreGive(s_lock);
}

static esp_err_t load_slot(nvs_handle_t handle, const char *key,
                           jr_memory_snapshot_t *out, bool *valid)
{
    *valid = false;
    size_t length = 0U;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &length);
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(*out)) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = nvs_get_blob(handle, key, out, &length);
    if (err == ESP_OK) {
        *valid = snapshot_valid(out);
        if (!*valid) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    return err;
}

static esp_err_t persist_snapshot(const jr_memory_snapshot_t *next,
                                  int target_slot)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(JR_MEMORY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    const char *key = target_slot == 0 ? JR_MEMORY_SLOT_0 : JR_MEMORY_SLOT_1;
    err = nvs_set_blob(handle, key, next, sizeof(*next));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static uint32_t next_generation(void)
{
    uint32_t generation = s_snapshot.generation + 1U;
    return generation == 0U ? 1U : generation;
}

static int next_slot(void)
{
    return s_active_slot == 0 ? 1 : 0;
}

static esp_err_t validation_error(jr_memory_validation_t result)
{
    switch (result) {
    case JR_MEMORY_VALID:
        return ESP_OK;
    case JR_MEMORY_KEY_TOO_LONG:
    case JR_MEMORY_VALUE_TOO_LONG:
        return ESP_ERR_INVALID_SIZE;
    case JR_MEMORY_SECRET_REJECTED:
        return ESP_ERR_NOT_ALLOWED;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static unsigned char ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int ascii_casecmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        const unsigned char ac = ascii_lower((unsigned char)*a++);
        const unsigned char bc = ascii_lower((unsigned char)*b++);
        if (ac != bc) {
            return ac < bc ? -1 : 1;
        }
    }
    return *a == *b ? 0 : (*a == '\0' ? -1 : 1);
}

static bool ascii_contains_ci(const char *haystack, const char *needle)
{
    if (*needle == '\0') {
        return true;
    }
    for (const char *h = haystack; *h != '\0'; ++h) {
        const char *a = h;
        const char *b = needle;
        while (*a != '\0' && *b != '\0' &&
               ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static int find_exact(const jr_memory_snapshot_t *snapshot, const char *key)
{
    for (size_t i = 0U; i < JR_MEMORY_MAX_ENTRIES; ++i) {
        if (snapshot->entries[i].used != 0U &&
            ascii_casecmp(snapshot->entries[i].key, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_recall(const jr_memory_snapshot_t *snapshot, const char *query)
{
    int exact = find_exact(snapshot, query);
    if (exact >= 0) {
        return exact;
    }
    for (size_t i = 0U; i < JR_MEMORY_MAX_ENTRIES; ++i) {
        if (snapshot->entries[i].used != 0U &&
            ascii_contains_ci(snapshot->entries[i].key, query)) {
            return (int)i;
        }
    }
    for (size_t i = 0U; i < JR_MEMORY_MAX_ENTRIES; ++i) {
        if (snapshot->entries[i].used != 0U &&
            ascii_contains_ci(snapshot->entries[i].value, query)) {
            return (int)i;
        }
    }
    return -1;
}

static void json_put_bytes(json_writer_t *writer, const char *data, size_t len)
{
    if (writer->failed) {
        return;
    }
    if (len > writer->cap - writer->used - 1U) {
        writer->failed = true;
        return;
    }
    memcpy(writer->out + writer->used, data, len);
    writer->used += len;
    writer->out[writer->used] = '\0';
}

static void json_put(json_writer_t *writer, const char *text)
{
    json_put_bytes(writer, text, strlen(text));
}

static void json_string(json_writer_t *writer, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    json_put(writer, "\"");
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        if (*p == '\"' || *p == '\\') {
            const char escaped[2] = {'\\', (char)*p};
            json_put_bytes(writer, escaped, sizeof(escaped));
        } else if (*p < 0x20U) {
            const char escaped[6] = {'\\', 'u', '0', '0', hex[*p >> 4U], hex[*p & 0x0FU]};
            json_put_bytes(writer, escaped, sizeof(escaped));
        } else {
            json_put_bytes(writer, (const char *)p, 1U);
        }
    }
    json_put(writer, "\"");
}

static void json_fact(json_writer_t *writer,
                      const jr_memory_entry_disk_t *entry)
{
    json_put(writer, "{\"key\":");
    json_string(writer, entry->key);
    json_put(writer, ",\"value\":");
    json_string(writer, entry->value);
    json_put(writer, "}");
}

static bool valid_query(const char *query)
{
    if (query == NULL || *query == '\0') {
        return false;
    }
    size_t len = 0U;
    bool non_space = false;
    for (; len <= JR_MEMORY_QUERY_MAX && query[len] != '\0'; ++len) {
        const unsigned char c = (unsigned char)query[len];
        if (c < 0x20U || c == 0x7FU) {
            return false;
        }
        if (c != ' ') {
            non_space = true;
        }
    }
    return len <= JR_MEMORY_QUERY_MAX && non_space;
}

esp_err_t jr_memory_init(void)
{
    esp_err_t err = lock_store();
    if (err != ESP_OK) {
        atomic_store(&s_last_error, err);
        return err;
    }
    if (s_ready) {
        unlock_store();
        return ESP_OK;
    }

    bool valid[2] = {false, false};
    esp_err_t slot_err[2] = {ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NVS_NOT_FOUND};
    nvs_handle_t handle;
    err = nvs_open(JR_MEMORY_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        slot_err[0] = load_slot(handle, JR_MEMORY_SLOT_0, &s_snapshot, &valid[0]);
        slot_err[1] = load_slot(handle, JR_MEMORY_SLOT_1, &s_work, &valid[1]);
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        atomic_store(&s_last_error, err);
        unlock_store();
        return err;
    }

    if (!valid[0] && !valid[1]) {
        esp_err_t serious = ESP_OK;
        for (size_t i = 0U; i < 2U; ++i) {
            if (slot_err[i] != ESP_OK && slot_err[i] != ESP_ERR_NVS_NOT_FOUND) {
                serious = slot_err[i];
                ESP_LOGE(TAG, "no recoverable snapshot; slot %u is invalid (%s)",
                         (unsigned)i, esp_err_to_name(slot_err[i]));
                break;
            }
        }
        if (serious != ESP_OK) {
            atomic_store(&s_last_error, serious);
            unlock_store();
            return serious;
        }
        snapshot_blank(&s_snapshot, 1U);
        s_active_slot = -1;
    } else if (valid[0] && valid[1]) {
        if (generation_newer(s_work.generation, s_snapshot.generation)) {
            s_snapshot = s_work;
            s_active_slot = 1;
        } else {
            s_active_slot = 0;
        }
    } else if (valid[0] || valid[1]) {
        s_active_slot = valid[1] ? 1 : 0;
        if (valid[1]) {
            s_snapshot = s_work;
        }
    }

    for (size_t i = 0U; i < 2U; ++i) {
        if (slot_err[i] != ESP_OK && slot_err[i] != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "ignored invalid snapshot slot %u (%s)",
                     (unsigned)i, esp_err_to_name(slot_err[i]));
        }
    }
    s_ready = true;
    atomic_store(&s_last_error, ESP_OK);
    ESP_LOGI(TAG, "ready: %u/%u facts, generation %lu",
             (unsigned)s_snapshot.count, (unsigned)JR_MEMORY_MAX_ENTRIES,
             (unsigned long)s_snapshot.generation);
    unlock_store();
    return ESP_OK;
}

esp_err_t jr_memory_store(const char *key, const char *value)
{
    const jr_memory_validation_t validation = jr_memory_validate_fact(key, value);
    if (validation != JR_MEMORY_VALID) {
        if (validation == JR_MEMORY_SECRET_REJECTED) {
            atomic_fetch_add(&s_rejected_secrets, 1U);
        }
        const esp_err_t err = validation_error(validation);
        atomic_store(&s_last_error, err);
        return err;
    }

    esp_err_t err = lock_store();
    if (err != ESP_OK) {
        atomic_store(&s_last_error, err);
        return err;
    }
    if (!s_ready) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    s_work = s_snapshot;
    int index = find_exact(&s_work, key);
    if (index < 0) {
        if (s_work.count >= JR_MEMORY_MAX_ENTRIES) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        for (size_t i = 0U; i < JR_MEMORY_MAX_ENTRIES; ++i) {
            if (s_work.entries[i].used == 0U) {
                index = (int)i;
                break;
            }
        }
        if (index < 0) {
            err = ESP_ERR_INVALID_STATE;
            goto out;
        }
        s_work.entries[index].used = 1U;
        ++s_work.count;
    }
    jr_memory_entry_disk_t *entry = &s_work.entries[index];
    memset(entry->key, 0, sizeof(entry->key));
    memset(entry->value, 0, sizeof(entry->value));
    memcpy(entry->key, key, strlen(key));
    memcpy(entry->value, value, strlen(value));
    s_work.generation = next_generation();
    s_work.crc32 = snapshot_crc(&s_work);

    const int target = next_slot();
    err = persist_snapshot(&s_work, target);
    if (err == ESP_OK) {
        s_snapshot = s_work;
        s_active_slot = target;
        atomic_fetch_add(&s_successful_writes, 1U);
    }

out:
    atomic_store(&s_last_error, err);
    unlock_store();
    return err;
}

esp_err_t jr_memory_recall(const char *query, char *out, size_t out_cap,
                           bool *found)
{
    if (!valid_query(query) || out == NULL || out_cap == 0U || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    *found = false;

    esp_err_t err = lock_store();
    if (err != ESP_OK) {
        atomic_store(&s_last_error, err);
        return err;
    }
    if (!s_ready) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    json_writer_t writer = {.out = out, .cap = out_cap};
    const int index = find_recall(&s_snapshot, query);
    if (index < 0) {
        json_put(&writer, "{\"found\":false}");
    } else {
        json_put(&writer, "{\"found\":true,\"fact\":");
        json_fact(&writer, &s_snapshot.entries[index]);
        json_put(&writer, "}");
        *found = true;
    }
    if (writer.failed) {
        out[0] = '\0';
        *found = false;
        err = ESP_ERR_INVALID_SIZE;
    } else {
        err = ESP_OK;
    }

out:
    atomic_store(&s_last_error, err);
    unlock_store();
    return err;
}

esp_err_t jr_memory_list(char *out, size_t out_cap, size_t *count)
{
    if (out == NULL || out_cap == 0U || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    *count = 0U;

    esp_err_t err = lock_store();
    if (err != ESP_OK) {
        atomic_store(&s_last_error, err);
        return err;
    }
    if (!s_ready) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    json_writer_t writer = {.out = out, .cap = out_cap};
    json_put(&writer, "[");
    bool first = true;
    for (size_t i = 0U; i < JR_MEMORY_MAX_ENTRIES; ++i) {
        if (s_snapshot.entries[i].used == 0U) {
            continue;
        }
        if (!first) {
            json_put(&writer, ",");
        }
        json_fact(&writer, &s_snapshot.entries[i]);
        first = false;
    }
    json_put(&writer, "]");
    if (writer.failed) {
        out[0] = '\0';
        err = ESP_ERR_INVALID_SIZE;
    } else {
        *count = s_snapshot.count;
        err = ESP_OK;
    }

out:
    atomic_store(&s_last_error, err);
    unlock_store();
    return err;
}

esp_err_t jr_memory_status(jr_memory_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_store();
    if (err != ESP_OK) {
        return err;
    }
    memset(out, 0, sizeof(*out));
    out->ready = s_ready;
    out->entries = s_ready ? s_snapshot.count : 0U;
    out->capacity = JR_MEMORY_MAX_ENTRIES;
    out->generation = s_ready ? s_snapshot.generation : 0U;
    out->successful_writes = atomic_load(&s_successful_writes);
    out->rejected_secrets = atomic_load(&s_rejected_secrets);
    out->last_error = (esp_err_t)atomic_load(&s_last_error);
    unlock_store();
    return ESP_OK;
}

esp_err_t jr_memory_clear(void)
{
    esp_err_t err = lock_store();
    if (err != ESP_OK) {
        atomic_store(&s_last_error, err);
        return err;
    }
    snapshot_blank(&s_work, s_ready ? next_generation() : 1U);
    const int old_slot = s_active_slot;
    const int target = s_ready ? next_slot() : 0;
    err = persist_snapshot(&s_work, target);
    if (err != ESP_OK) {
        goto out;
    }
    s_snapshot = s_work;
    s_active_slot = target;
    s_ready = true;
    atomic_fetch_add(&s_successful_writes, 1U);

    /* The committed blank generation already makes clear durable.  Retiring
     * the old slot is a second transaction; if it fails, no fact resurrects
     * because boot still selects the newer blank snapshot. */
    if (old_slot >= 0 && old_slot != target) {
        nvs_handle_t handle;
        esp_err_t retire_err = nvs_open(JR_MEMORY_NAMESPACE, NVS_READWRITE, &handle);
        if (retire_err == ESP_OK) {
            retire_err = nvs_erase_key(handle,
                                       old_slot == 0 ? JR_MEMORY_SLOT_0 : JR_MEMORY_SLOT_1);
            if (retire_err == ESP_OK || retire_err == ESP_ERR_NVS_NOT_FOUND) {
                retire_err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
        if (retire_err != ESP_OK) {
            err = retire_err;
        }
    } else if (old_slot < 0) {
        /* Recovery after an unreadable store: slot 0 now contains a valid
         * blank snapshot, so retire any stale/corrupt slot 1. */
        nvs_handle_t handle;
        esp_err_t retire_err = nvs_open(JR_MEMORY_NAMESPACE, NVS_READWRITE, &handle);
        if (retire_err == ESP_OK) {
            retire_err = nvs_erase_key(handle, JR_MEMORY_SLOT_1);
            if (retire_err == ESP_OK || retire_err == ESP_ERR_NVS_NOT_FOUND) {
                retire_err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
        if (retire_err != ESP_OK) {
            err = retire_err;
        }
    }

out:
    atomic_store(&s_last_error, err);
    unlock_store();
    return err;
}
