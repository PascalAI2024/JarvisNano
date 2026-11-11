# Crash forensics

**What it is** — The reset reason read at boot plus the ESP-IDF flash coredump (`espcoredump`, IDF v5.5.4, ELF format, partition `coredump`, 64 KiB), exposed over HTTP so an unexplained restart can be read after the fact.

**How we use it here** — `crash_note_boot()` (`main/http_routes.c`) logs `boot: reset reason=<name>` as the first line of `app_main` and `/api/device/health` carries it as `"reset"`. `GET /api/diag/crash` reports the reset reason and the stored dump's summary. Nothing erases the dump except an explicit `POST /api/diag/crash?clear=1`: the dump survives OTA and reboots, so read it before clearing.

---

## Routes

| Route | Auth | Result |
|-------|------|--------|
| `GET /api/device/health` | as before | new key `reset` |
| `GET /api/diag/crash` | agent auth, read-only | JSON below |
| `GET /api/diag/coredump.bin` | agent auth, read-only | raw stored image, 4 KB chunks; 404 when absent |
| `POST /api/diag/crash?clear=1` | agent auth + `X-JarvisNano-Control: 1` | `esp_core_dump_image_erase()` |

`/api/diag/crash` keys: `reset`, `dump_present`, `dump_valid`, `dump_bytes`, `summary`. `summary` is `null` without a valid dump, else: `task`, `exc_pc`, `exc_cause`, `exc_vaddr`, `elf_sha`, `version`, `bt_corrupted`, `bt_depth`, `bt` (hex strings, at most 16).

Reset names: poweron, ext, sw, panic, int_wdt, task_wdt, wdt, deepsleep, brownout, sdio, usb, jtag, unknown.

Host side: `scripts/jarvisctl.py crash` (summary), `crash dump [out.bin]` (raw image), `crash clear`.

## Turning a backtrace into lines

The ELF must be the crashed image's own: compare `elf_sha` with the prefix of `esp_app_desc` SHA of the build you hold (`xtensa-esp32s3-elf-readelf` or `idf.py size`); a different build gives plausible but wrong lines. Run inside the `espressif/idf:v5.5.4` image:

```
xtensa-esp32s3-elf-addr2line -pfiaC -e build/jarvisrobot_v5.elf <exc_pc> <bt...>
```

The raw `coredump.bin` decodes with `esp-coredump info_corefile --core coredump.bin --core-format raw build/jarvisrobot_v5.elf` (ELF-format dump; flag set not fetch-verified).

---

## Findings & gotchas

**[2026-11-11] Heap, not stack**
`esp_core_dump_summary_t` (about 300 bytes with the Xtensa extra info) and the JSON buffer are PSRAM allocations: the httpd task has about 1500 bytes of free stack.

**[2026-11-11] The summary needs no new Kconfig**
`esp_core_dump_get_summary()` is compiled when `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` and `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF` are both set, which `sdkconfig.defaults:123` already does. `elf_sha` is `CONFIG_APP_RETRIEVE_LEN_ELF_SHA` (9) characters.

**[2026-11-11] Reset reason is not a crash proof**
`panic`, `int_wdt`, `task_wdt` and `brownout` point at a fault; `sw` is `esp_restart`, `deepsleep` a wake. A dump in flash may predate the current boot: compare `elf_sha` and the device uptime before trusting it.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `main/http_routes.c` (`crash_get_handler`, `crash_clear_handler`, `coredump_bin_handler`) | the routes |
| `sdkconfig.defaults:123` | coredump to flash, ELF format |
| `partitions_32MB.csv:20` | the 64 KiB `coredump` partition |
| IDF `components/espcoredump/include/esp_core_dump.h` | summary and image API |

## Open questions

- Not exercised on a device: whether the 64 KiB partition holds a full dump of this firmware, and what the real summary reads.

## See also

- [build-toolchain.md](./build-toolchain.md) — the Docker build that produces the ELF.
