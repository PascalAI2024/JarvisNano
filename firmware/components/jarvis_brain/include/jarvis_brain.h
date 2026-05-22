/*
 * jarvis_brain - persistent on-device "self" for the JarvisRobot ESP32-S3.
 *
 * Gives JARVIS a continuous local identity + memory that lives on the SD card
 * (survives reboots and reflashes; the SD is not wiped by flash). At session
 * start the voice path loads identity + a bounded tail of memory into the LLM
 * system instruction; after the session it can append learnings back.
 *
 * Storage layout under <base_dir>/brain/ :
 *   identity.md  - persona / who-he-is. Seeded on first boot, user-editable.
 *   memory.log   - append-only timestamped interaction/learning notes,
 *                  size-capped (rotates once to memory.1.log so it can't fill
 *                  the card).
 *   facts.md     - optional curated durable facts (read if present).
 *
 * Design contract: FAIL-SOFT EVERYWHERE. If the SD or <base_dir>/brain cannot
 * be read or written, init/remember return non-fatal and load_context falls
 * back to a built-in default persona. The voice session must NEVER hang or
 * crash because of the brain.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the on-device brain.
 *
 * Creates <base_dir>/brain/ and seeds identity.md with the default JARVIS
 * persona if it does not already exist. MUST be called AFTER the SD card is
 * mounted (next to jarvis_logger_init), but it is safe to call when the SD is
 * absent.
 *
 * Fail-soft: ALWAYS returns ESP_OK so a brain/SD problem can never abort the
 * boot path. On any filesystem failure the brain simply stays "not ready"
 * (jarvis_brain_ready() == false) and load_context() serves the built-in
 * default persona.
 *
 * @param base_dir Mount root, e.g. "/sdcard". NULL defaults to "/sdcard".
 * @return ESP_OK always.
 */
esp_err_t jarvis_brain_init(const char *base_dir);

/**
 * @brief Build the LLM system-instruction text from identity + recent memory.
 *
 * Composes identity.md, then a bounded tail of facts.md + memory.log
 * (most-recent notes), capped to a sane size (<= ~2 KB) so it fits a system
 * instruction without bloating the WSS setup frame. The output is ALWAYS
 * NUL-terminated.
 *
 * Fail-soft: ALWAYS returns ESP_OK. If the brain is not ready or identity.md
 * cannot be read, the built-in default persona is copied into @p out so the
 * voice path always has a usable instruction.
 *
 * @param out    Destination buffer (must be non-NULL, out_sz >= 1).
 * @param out_sz Size of @p out in bytes (including space for the NUL).
 * @return ESP_OK always (ESP_ERR_INVALID_ARG only if out==NULL or out_sz==0).
 */
esp_err_t jarvis_brain_load_context(char *out, size_t out_sz);

/**
 * @brief Append a learned fact / interaction note to persistent memory.
 *
 * Best-effort: writes a single timestamped line to <base_dir>/brain/memory.log
 * (synchronous fopen/fputs/fclose). Rotates the log once if it exceeds the
 * size cap. Never blocks on a large read.
 *
 * Fail-soft: returns ESP_OK on success, ESP_FAIL if the note could not be
 * persisted (e.g. SD absent) — callers may ignore the result. NEVER aborts.
 *
 * @param note Text to append (NUL-terminated). NULL/empty is a no-op (ESP_OK).
 * @return ESP_OK if written (or no-op), ESP_FAIL if it could not be persisted.
 */
esp_err_t jarvis_brain_remember(const char *note);

/**
 * @brief True if the brain directory is present and usable on the SD card.
 *        false means load_context() will serve the built-in default persona.
 */
bool jarvis_brain_ready(void);

#ifdef __cplusplus
}
#endif
