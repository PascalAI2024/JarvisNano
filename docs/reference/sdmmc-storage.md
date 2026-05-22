# SDMMC Storage

**What it is** — A microSD card interface exposed as a FAT filesystem mounted at `/sdcard` via the ESP-IDF VFS layer. On this board it uses the SDMMC peripheral in 1-bit bus mode.

**How we use it here** — The SD card provides a writable filesystem for logs, user data, and any assets too large to fit in the flash partition layout. Access via standard `fopen`/`fread`/`fwrite` after the VFS mount.

---

## Configuration (from `board_devices.yaml`)

The `fs_sdcard` device is declared in `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:77-90`:

```yaml
- name: fs_sdcard
  type: fs_fat
  sub_type: sdmmc
  version: default
  config:
    vfs_config:
      format_if_mount_failed: true
    sub_config:
      bus_width: 1
      slot_flags: "SDMMC_SLOT_FLAG_INTERNAL_PULLUP"
      pins:
        clk: 2    # GPIO2
        cmd: 1    # GPIO1
        d0:  3    # GPIO3
```

Pin assignment confirmed against the Waveshare BSP header:
- `BSP_SD_D0` = `GPIO_NUM_3`
- `BSP_SD_CMD` = `GPIO_NUM_1`
- `BSP_SD_CLK` = `GPIO_NUM_2`

Source: `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:77-90`; Waveshare BSP `esp32_s3_touch_amoled_1_75.h` (`BSP_SD_D0/CMD/CLK` macros, verified).

---

## Findings & gotchas

**[2026-05-21] `format_if_mount_failed: true` — SD cards with non-FAT filesystems are auto-reformatted**

The `format_if_mount_failed` flag means the firmware will silently reformat a blank or non-FAT-formatted card on first mount rather than aborting board init. This is intentional: it allows any microSD card to work without pre-formatting. Be aware that an ext4 or exFAT card inserted into a dev device will be wiped.

Source: `board_devices.yaml:83` (flag confirmed in yaml); Waveshare BSP `bsp_sdcard_mount()` (conditional compile using `CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL`, verified).

**[2026-05-21] Probe free space with `esp_vfs_fat_info`**

To check whether the SD card is mounted and usable:
```c
uint64_t total_bytes, free_bytes;
esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes);
```

Returns `ESP_OK` if the VFS mount is healthy. Use this as a diagnostic before attempting file I/O.

**[2026-05-21] Internal-flash FATFS is a separate mount**

The board also has an internal-flash FAT partition. It is a separate VFS mount — not `/sdcard`. Do not confuse the two. Internal flash is used for the `emote` partition (assets) and NVS; the SD card is `/sdcard`.

**[2026-05-21] 1-bit bus mode — slower but more reliable**

The SD card uses 1-bit SDMMC (D1–D3 are `GPIO_NUM_NC`). This halves theoretical throughput compared to 4-bit mode but is more reliable on boards without dedicated SD routing. For audio/log workloads, 1-bit is sufficient.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:77-90` | Device declaration, pin assignments, `format_if_mount_failed`. Ground truth. |
| [Waveshare BSP `esp32_s3_touch_amoled_1_75.c`](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/master/bsp/esp32_s3_touch_amoled_1_75/esp32_s3_touch_amoled_1_75.c) | `bsp_sdcard_mount()` — reference implementation. |
| [ESP-IDF SDMMC docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/sdmmc.html) | `esp_vfs_fat_sdmmc_mount`, slot config, `esp_vfs_fat_info`. |

---

## Open questions

- Is there a `max_files` limit configured on the FAT mount? The Waveshare BSP uses `max_files = 5` — what does the board-manager device use?
- Is the SD card mount triggered automatically by the board manager on boot, or does the application need to explicitly call a mount function?

---

## See also

- [waveshare-amoled-175.md](./waveshare-amoled-175.md) — full board hardware overview.
- [board-manager.md](./board-manager.md) — how `fs_sdcard` becomes a runtime-accessible device.
- [asset-pipeline.md](./asset-pipeline.md) — internal-flash `emote` partition (distinct from SD card).
