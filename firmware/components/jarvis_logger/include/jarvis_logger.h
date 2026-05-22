/*
 * jarvis_logger - persistent ESP_LOG tee to rotating files on the SD card.
 *
 * Installs a non-blocking esp_log_set_vprintf() hook that copies every log
 * line into a PSRAM ring buffer; a low-priority writer task drains the ring to
 * a rotating log file under <base_dir>/logs/. The original vprintf handler is
 * always invoked, so UART/serial logging is unaffected.
 *
 * Design constraints (see README.md):
 *   - The hook NEVER touches the filesystem; it only fills the ring buffer.
 *   - Producer never blocks: on ring-full the new line is dropped and counted.
 *   - Recursion-safe: logs emitted by the writer task (FATFS/SDMMC chatter)
 *     are forwarded to UART only and never re-enter the ring.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise persistent logging.
 *
 * Creates <base_dir>/logs/, opens the active log file (append), starts the
 * writer task, and installs the esp_log vprintf hook. MUST be called AFTER the
 * SD card is mounted (e.g. after esp_vfs_fat_sdmmc_mount returns ESP_OK).
 *
 * @param base_dir Mount root, e.g. "/sdcard". NULL defaults to "/sdcard".
 * @return ESP_OK on success;
 *         ESP_ERR_INVALID_STATE if already initialised;
 *         ESP_ERR_NO_MEM if the PSRAM ring or primitives cannot be allocated;
 *         ESP_FAIL if the log directory/file cannot be opened.
 */
esp_err_t jarvis_logger_init(const char *base_dir);

/**
 * @brief Uninstall the hook, stop the writer task, flush and close the file.
 *        Safe to call when not initialised (no-op).
 */
void jarvis_logger_deinit(void);

/**
 * @brief Absolute path to the active log file (e.g. "/sdcard/logs/jarvis.log").
 * @return Path string, or NULL if not initialised. Stable for the lifetime of
 *         the logger; the HTTP handler may open it read-only at any time.
 */
const char *jarvis_logger_current_path(void);

/**
 * @brief Diagnostics counters.
 *
 * @param out_dropped       Lines dropped due to ring-full (may be NULL).
 * @param out_bytes_written Total bytes flushed to disk so far (may be NULL).
 */
void jarvis_logger_get_stats(uint32_t *out_dropped, uint32_t *out_bytes_written);

/**
 * @brief Register the GET /api/logs route on a running HTTP server.
 *
 * Streams the tail of the active log file as text/plain (query ?tail=N bytes,
 * default 16 KB, capped 256 KB). Call once from the http_server route setup.
 *
 * @param server An httpd_handle_t (typed void* to keep this header free of the
 *               esp_http_server include). Must be a valid, started server.
 * @return ESP_OK on success, or the underlying httpd registration error.
 */
esp_err_t jarvis_logger_register_http(void *server);

#ifdef __cplusplus
}
#endif
