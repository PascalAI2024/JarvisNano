/*
 * jarvis_logger.c - non-blocking ESP_LOG tee to rotating SD-card files.
 *
 * Threading model
 * ---------------
 *  Producer (the vprintf hook, runs in WHATEVER task is logging):
 *      vsnprintf into a small stack buffer (via va_copy), then under a
 *      portMUX spinlock memcpy the bytes into the PSRAM ring and advance the
 *      head. On ring-full the line is dropped and s_dropped incremented.
 *      The original vprintf is ALWAYS called so UART logging keeps working.
 *      A task notification is sent to the writer OUTSIDE the critical section.
 *
 *  Consumer (s_writer_task, low priority):
 *      Waits on a task notification with a timeout (periodic flush). Drains
 *      the ring to the open FILE*, fflush+fsync periodically, and rotates the
 *      file when it exceeds JL_ROTATE_BYTES. Logs emitted by FATFS/SDMMC while
 *      this task writes are detected via the current-task-handle guard and are
 *      forwarded to UART only -- they never re-enter the ring (no deadlock).
 */

#include "jarvis_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>   /* fsync() */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

/* ---- Tunables ---------------------------------------------------------- */
#define JL_RING_SIZE        (32 * 1024)     /* PSRAM ring buffer bytes        */
#define JL_LINE_MAX         256             /* stack scratch per log line     */
#define JL_ROTATE_BYTES     (1024 * 1024)   /* rotate when file exceeds 1 MB  */
#define JL_KEEP_FILES       4               /* jarvis.1.log .. jarvis.4.log   */
#define JL_WRITER_STACK     4096
#define JL_WRITER_PRIO      2
#define JL_WRITER_CORE      0               /* keep off the audio/Wi-Fi core  */
#define JL_FLUSH_PERIOD_MS  2000            /* periodic flush + rotate check  */
#define JL_FLUSH_EVERY      (8 * 1024)      /* fsync after this many bytes    */

#define JL_PATH_MAX         128
/* Directory buffer is capped well below JL_PATH_MAX so the compiler can prove
 * "<dir>/jarvis.N.log" never overruns the full-path buffers (-Werror=format-truncation). */
#define JL_DIR_MAX          96
#define JL_DEFAULT_BASE     "/sdcard"
#define JL_LOG_SUBDIR       "logs"
#define JL_LOG_NAME         "jarvis.log"

static const char *TAG = "jarvis_logger";

/* ---- State ------------------------------------------------------------- */
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t       *s_ring;            /* PSRAM ring buffer                 */
static volatile size_t s_head;           /* producer write index             */
static volatile size_t s_tail;           /* consumer read index              */
/* Bytes currently queued = (s_head - s_tail) modulo JL_RING_SIZE.           */

static vprintf_like_t s_prev_vprintf;    /* original handler (UART)           */
static TaskHandle_t   s_writer_task;
static volatile bool  s_running;
static FILE          *s_file;
static bool           s_file_open;
static size_t         s_file_bytes;      /* bytes in the current file         */

static char           s_dir_path[JL_DIR_MAX];    /* "<base>/logs"            */
static char           s_cur_path[JL_PATH_MAX];   /* "<base>/logs/jarvis.log" */

static _Atomic uint32_t s_dropped;
static _Atomic uint32_t s_bytes_written;

/* ---- Ring helpers (called under s_mux, except where noted) ------------- */
static inline size_t ring_used(void)
{
    size_t h = s_head, t = s_tail;
    return (h >= t) ? (h - t) : (JL_RING_SIZE - t + h);
}

static inline size_t ring_free(void)
{
    /* keep 1 byte unused so head==tail unambiguously means "empty" */
    return JL_RING_SIZE - 1 - ring_used();
}

/* ---- The log hook (runs in arbitrary, possibly high-prio, tasks) ------- */
static int jl_vprintf(const char *fmt, va_list args)
{
    /* Recursion guard: if the writer task itself logs (FATFS/SDMMC chatter),
     * forward to UART only and skip the ring -- never re-enter a file write. */
    if (s_writer_task && xTaskGetCurrentTaskHandle() == s_writer_task) {
        return s_prev_vprintf ? s_prev_vprintf(fmt, args) : 0;
    }

    /* Format into a small stack buffer using a copy of the va_list so the
     * original remains valid for the forwarded call below. */
    char buf[JL_LINE_MAX];
    va_list ap_copy;
    va_copy(ap_copy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap_copy);
    va_end(ap_copy);

    if (s_ring && n > 0) {
        /* vsnprintf writes at most sizeof(buf)-1 real chars plus a NUL and
         * returns the would-have-been length. Cap at sizeof(buf)-1 so we never
         * splice the trailing NUL into the ring (it would break tail/cat). */
        size_t cap = sizeof(buf) - 1;
        size_t len = ((size_t)n > cap) ? cap : (size_t)n;
        bool notify = false;
        portENTER_CRITICAL(&s_mux);
        if (ring_free() >= len) {
            size_t h = s_head;
            size_t first = JL_RING_SIZE - h;
            if (first >= len) {
                memcpy(s_ring + h, buf, len);
            } else {
                memcpy(s_ring + h, buf, first);
                memcpy(s_ring, buf + first, len - first);
            }
            s_head = (h + len) % JL_RING_SIZE;
            notify = true;
        } else {
            atomic_fetch_add(&s_dropped, 1);   /* ring full -> drop the line */
        }
        portEXIT_CRITICAL(&s_mux);

        /* Notify OUTSIDE the critical section (may cause a context switch). */
        if (notify && s_writer_task) {
            xTaskNotifyGive(s_writer_task);
        }
    }

    /* Always forward to the original handler with the untouched va_list. */
    return s_prev_vprintf ? s_prev_vprintf(fmt, args) : n;
}

/* ---- File management (writer-task context only) ------------------------ */
static void jl_close_file(void)
{
    if (s_file) {
        fflush(s_file);
        fclose(s_file);
        s_file = NULL;
    }
    s_file_open = false;
}

/* Rotate jarvis.log -> jarvis.1.log -> ... keep up to JL_KEEP_FILES. */
static void jl_rotate(void)
{
    jl_close_file();

    char from[JL_PATH_MAX];
    char to[JL_PATH_MAX];

    /* delete the oldest */
    snprintf(to, sizeof(to), "%s/jarvis.%d.log", s_dir_path, JL_KEEP_FILES);
    remove(to);

    /* shift jarvis.(n-1).log -> jarvis.n.log, down to jarvis.1.log */
    for (int i = JL_KEEP_FILES - 1; i >= 1; --i) {
        snprintf(from, sizeof(from), "%s/jarvis.%d.log", s_dir_path, i);
        snprintf(to,   sizeof(to),   "%s/jarvis.%d.log", s_dir_path, i + 1);
        rename(from, to);   /* missing source is harmless */
    }

    /* jarvis.log -> jarvis.1.log */
    snprintf(to, sizeof(to), "%s/jarvis.1.log", s_dir_path);
    rename(s_cur_path, to);

    /* reopen a fresh active file */
    s_file = fopen(s_cur_path, "a");
    s_file_open = (s_file != NULL);
    s_file_bytes = 0;
}

/* Drain the ring to the file. Returns bytes written this pass. */
static size_t jl_drain(void)
{
    size_t written = 0;
    for (;;) {
        /* Snapshot a contiguous span under the lock, copy out, release. */
        uint8_t local[512];
        size_t  chunk = 0;

        portENTER_CRITICAL(&s_mux);
        size_t used = ring_used();
        if (used == 0) {
            portEXIT_CRITICAL(&s_mux);
            break;
        }
        size_t t = s_tail;
        size_t contig = JL_RING_SIZE - t;          /* up to wrap point */
        chunk = used < contig ? used : contig;
        if (chunk > sizeof(local)) {
            chunk = sizeof(local);
        }
        memcpy(local, s_ring + t, chunk);
        s_tail = (t + chunk) % JL_RING_SIZE;
        portEXIT_CRITICAL(&s_mux);

        /* File I/O happens OUTSIDE the critical section. Fail soft. */
        if (s_file_open && s_file) {
            size_t w = fwrite(local, 1, chunk, s_file);
            written      += w;
            s_file_bytes += w;
            atomic_fetch_add(&s_bytes_written, (uint32_t)w);
        }
        /* If the file is gone, we still consumed the ring (bytes are dropped
         * from disk but the producer stays unblocked). */
    }
    return written;
}

static void jl_writer_task(void *arg)
{
    (void)arg;
    size_t since_sync = 0;
    int64_t last_check = esp_timer_get_time();

    while (s_running) {
        /* Wait for data or the periodic flush tick. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(JL_FLUSH_PERIOD_MS));

        size_t w = jl_drain();
        since_sync += w;

        int64_t now = esp_timer_get_time();
        bool periodic = (now - last_check) >= (int64_t)JL_FLUSH_PERIOD_MS * 1000;

        if (s_file_open && s_file && (since_sync >= JL_FLUSH_EVERY || periodic)) {
            fflush(s_file);
            int fd = fileno(s_file);
            if (fd >= 0) {
                fsync(fd);
            }
            since_sync = 0;
        }

        if (periodic) {
            last_check = now;
            /* Rotate check (writer-context only, recursion-guarded). */
            if (s_file_bytes >= JL_ROTATE_BYTES) {
                jl_rotate();
            }
            /* If the SD vanished earlier, attempt a soft reopen. */
            if (!s_file_open) {
                s_file = fopen(s_cur_path, "a");
                s_file_open = (s_file != NULL);
            }
        }
    }

    /* Final drain + close on shutdown. */
    jl_drain();
    if (s_file_open && s_file) {
        fflush(s_file);
    }
    jl_close_file();
    s_writer_task = NULL;
    vTaskDelete(NULL);
}

/* ---- Public API -------------------------------------------------------- */
esp_err_t jarvis_logger_init(const char *base_dir)
{
    if (s_running || s_ring) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!base_dir) {
        base_dir = JL_DEFAULT_BASE;
    }

    snprintf(s_dir_path, sizeof(s_dir_path), "%s/%s", base_dir, JL_LOG_SUBDIR);
    snprintf(s_cur_path, sizeof(s_cur_path), "%s/%s", s_dir_path, JL_LOG_NAME);

    /* mkdir <base>/logs (ignore EEXIST). */
    if (mkdir(s_dir_path, 0777) != 0) {
        struct stat st;
        if (stat(s_dir_path, &st) != 0) {
            ESP_LOGE(TAG, "cannot create %s", s_dir_path);
            return ESP_FAIL;
        }
    }

    /* Open the active file (append). */
    s_file = fopen(s_cur_path, "a");
    if (!s_file) {
        ESP_LOGE(TAG, "cannot open %s", s_cur_path);
        return ESP_FAIL;
    }
    s_file_open = true;
    {
        struct stat st;
        s_file_bytes = (stat(s_cur_path, &st) == 0) ? (size_t)st.st_size : 0;
    }

    /* Ring buffer in PSRAM. */
    s_ring = heap_caps_malloc(JL_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGE(TAG, "no PSRAM for %d-byte ring", JL_RING_SIZE);
        jl_close_file();
        return ESP_ERR_NO_MEM;
    }
    s_head = s_tail = 0;
    atomic_store(&s_dropped, 0);
    atomic_store(&s_bytes_written, 0);

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(
        jl_writer_task, "jl_writer", JL_WRITER_STACK, NULL,
        JL_WRITER_PRIO, &s_writer_task, JL_WRITER_CORE);
    if (ok != pdPASS) {
        s_running = false;
        heap_caps_free(s_ring);
        s_ring = NULL;
        jl_close_file();
        return ESP_ERR_NO_MEM;
    }

    /* Install the hook LAST and capture the previous handler (default vprintf).
     * From here on every ESP_LOGx is teed to the ring + forwarded to UART. */
    s_prev_vprintf = esp_log_set_vprintf(jl_vprintf);

    ESP_LOGI(TAG, "persistent logging -> %s (ring %dK, rotate %dK x%d)",
             s_cur_path, JL_RING_SIZE / 1024, JL_ROTATE_BYTES / 1024,
             JL_KEEP_FILES);
    return ESP_OK;
}

void jarvis_logger_deinit(void)
{
    if (!s_running && !s_ring) {
        return;
    }

    /* Restore the original handler first so no new lines enter the ring.
     * NOTE: esp_log_set_vprintf is an atomic pointer swap, not a quiesce. A
     * producer that already entered jl_vprintf may still be mid-memcpy on the
     * ring when we free it below. deinit is only expected on full teardown, so
     * this race is accepted; do not call deinit during normal operation. */
    if (s_prev_vprintf) {
        esp_log_set_vprintf(s_prev_vprintf);
        s_prev_vprintf = NULL;
    }

    /* Ask the writer task to finish; it final-drains and self-deletes. */
    s_running = false;
    if (s_writer_task) {
        xTaskNotifyGive(s_writer_task);
        /* Wait for it to clear the handle (it sets s_writer_task = NULL). */
        for (int i = 0; i < 200 && s_writer_task; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (s_ring) {
        heap_caps_free(s_ring);
        s_ring = NULL;
    }
    s_head = s_tail = 0;
}

const char *jarvis_logger_current_path(void)
{
    return (s_ring || s_file_open) ? s_cur_path : NULL;
}

void jarvis_logger_get_stats(uint32_t *out_dropped, uint32_t *out_bytes_written)
{
    if (out_dropped) {
        *out_dropped = atomic_load(&s_dropped);
    }
    if (out_bytes_written) {
        *out_bytes_written = atomic_load(&s_bytes_written);
    }
}
