# jarvis_logger

Persistent logging component for the JarvisRobot ESP32-S3 firmware. Tees **all**
`ESP_LOGx` output to rotating files on the SD card (`/sdcard`) without blocking
the tasks that emit logs.

## How it works

`esp_log_set_vprintf()` installs a hook that runs **synchronously in whatever
task is logging** — including high-priority Wi-Fi/audio tasks and the SDMMC
driver itself. SD writes can stall tens of ms, so the hook must never touch the
filesystem. The design:

```
ESP_LOGx (any task)            jl_writer_task (prio 2, core 0)
  │ jl_vprintf hook                   │
  ├─ vsnprintf -> stack buf           │
  ├─ memcpy -> PSRAM ring (spinlock)  │  ulTaskNotifyTake (2 s timeout)
  ├─ xTaskNotifyGive (writer) ────────┤  drain ring -> FILE* (fwrite)
  └─ forward to prev vprintf (UART)   │  fflush + fsync periodically
                                      └─ rotate at 1 MB
```

- **Non-blocking hook** — formats into a stack buffer (via `va_copy`, so the
  original `va_list` is preserved for forwarding), copies into a PSRAM ring
  under a `portMUX_TYPE` spinlock, then notifies the writer **outside** the
  critical section. Never writes to the file.
- **UART preserved** — the previous handler returned by `esp_log_set_vprintf`
  (default `&vprintf`) is always invoked.
- **PSRAM ring** — 32 KB via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. On
  ring-full the new line is **dropped** and an atomic counter incremented; the
  producer never blocks.
- **Recursion guard** — FATFS/SDMMC emit their own `ESP_LOGx` while the writer
  is writing. The hook checks `xTaskGetCurrentTaskHandle() == s_writer_task`;
  if true it forwards to UART only and skips the ring (no deadlock).
- **Writer task** — low priority (2), pinned to core 0, 4 KB stack. Drains the
  ring, `fflush`+`fsync` every ~8 KB or every 2 s, rotates at 1 MB.
- **Rotation** — `jarvis.log` → `jarvis.1.log` → … keep `jarvis.4.log`, oldest
  deleted.
- **Fail-soft** — if `fopen` fails or the SD disappears, writes are dropped and
  counted; the device never crashes. A soft reopen is retried each flush tick.

## Public API (`include/jarvis_logger.h`)

```c
esp_err_t   jarvis_logger_init(const char *base_dir);   // "/sdcard"; NULL -> "/sdcard"
void        jarvis_logger_deinit(void);
const char *jarvis_logger_current_path(void);           // active file path, or NULL
void        jarvis_logger_get_stats(uint32_t *dropped, uint32_t *bytes_written);
```

`jarvis_logger_init` creates `<base_dir>/logs/`, opens `jarvis.log` (append),
starts the writer task, and installs the hook. Returns `ESP_ERR_INVALID_STATE`
if already initialised, `ESP_ERR_NO_MEM` if the PSRAM ring/task can't be
allocated, `ESP_FAIL` if the directory/file can't be opened.

## Tunables (top of `src/jarvis_logger.c`)

| Macro | Default | Meaning |
|-------|---------|---------|
| `JL_RING_SIZE` | 32 KB | PSRAM ring buffer size |
| `JL_LINE_MAX` | 256 | Max formatted bytes per log line |
| `JL_ROTATE_BYTES` | 1 MB | Rotate threshold |
| `JL_KEEP_FILES` | 4 | Rotated files kept (`jarvis.1..4.log`) |
| `JL_WRITER_PRIO` / `JL_WRITER_CORE` | 2 / 0 | Writer task priority / core |
| `JL_FLUSH_PERIOD_MS` | 2000 | Periodic flush + rotate-check interval |
| `JL_FLUSH_EVERY` | 8 KB | fsync after this many bytes |

## What the orchestrator must wire (NOT done by this component)

1. **`main.c`** — call `jarvis_logger_init("/sdcard")` **after** the SD card is
   mounted (after `esp_vfs_fat_sdmmc_mount` / wherever `/sdcard` becomes
   available — see `APP_SDCARD_PATH` in `main/main.c`). Logs emitted before init
   go to UART only, which is fine. Add `jarvis_logger` to main's `REQUIRES`.

2. **`http_server`** — move `http_logs_handler.c.draft` into the http_server
   component (e.g. `http_server_logs_api.c`), add `jarvis_logger` to that
   component's `REQUIRES`, and register the route next to the existing
   config/status routes (`http_server_register_config_routes` in
   `http_server_config_api.c`):

   ```c
   { .uri = "/api/logs", .method = HTTP_GET, .handler = jarvis_logs_get_handler }
   ```

   `GET /api/logs?tail=N` streams the last `N` bytes (default 16 KB, cap 256 KB)
   of the active log file as `text/plain`.

No `bootstrap.sh` or build-toolchain changes are required — this is a pure new
component picked up automatically by the IDF component manager.
