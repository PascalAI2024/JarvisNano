# JarvisRobot Reference Knowledge Base

Organized, navigable reference pages for the ESP32-S3-Touch-AMOLED-1.75 firmware project. Each page covers one system area: what it is, how we use it, hard-won findings with dates, primary sources with file:line citations, and open questions.

**Capture rule:** After any research pass, record findings in the matching page below using `_TEMPLATE.md`. Cite primary sources (GitHub URL or `file:line`). Never put secrets, API keys, internal URLs, Wi-Fi credentials, device MACs/IPs, or bearer tokens in reference pages or anywhere in this repo.

---

## Pages

| Page | What it covers |
|------|---------------|
| [board-manager.md](./board-manager.md) | `esp_board_manager` device handle API, the double-deref gotcha, generated code files |
| [build-toolchain.md](./build-toolchain.md) | IDF v5.5.4 Docker image, `idf-component-manager==2.4.10` pin, `esp-bmgr-assist==0.5.0` pin, sdkconfig regeneration. Live recipe is `docs/BUILD.md` (`build-v5.sh`). |
| [waveshare-amoled-175.md](./waveshare-amoled-175.md) | Board hardware summary (CO5300/CST9217/ES8311/ES7210/AXP2101), GPIO table, AXP2101 init-skip, QSPI flash mode |
| [audio-es8311-es7210.md](./audio-es8311-es7210.md) | Codec chain, `esp_codec_dev` volume curve (0 dB ceiling), soft-knee limiter, choppy-audio PSRAM frame queue, 16kHz/24kHz I/O rates |
| [display-emote-gfx.md](./display-emote-gfx.md) | `esp_emote_gfx` widget API, proven absence of `gfx_canvas`, why runtime CPU-drawn buffers produce a spiral, sanctioned reactive-face paths |
| [gemini-live-api.md](./gemini-live-api.md) | Overlay-era Gemini notes. Prefer [gemini-live-api-v5.md](./gemini-live-api-v5.md) + header auth in `main.c`. |
| [gemini-live-api-v5.md](./gemini-live-api-v5.md) | v5 RealtimeVoiceClient, `thinkingLevel`, model ids |
| [aec-barge-in.md](./aec-barge-in.md) | AEC / barge-in design |
| [jarvismcp-bridge.md](./jarvismcp-bridge.md) | Function-calling gateway to the JarvisMCP `/act` endpoint, three-file NVS registration pattern, 15-char NVS key limit |
| [llm-config.md](./llm-config.md) | `llm_profile` protocol enum (not a vendor name), boot-loop recovery via `esptool erase-region`, full config field table |
| [asset-pipeline.md](./asset-pipeline.md) | EAF/AAF format, `gen_mascot_eaf.py` local encoder, `esp_mmap_assets` packer, silent `"lack":true` blank face, `emote_assets.bin` build flow |
| [sdmmc-storage.md](./sdmmc-storage.md) | SD card mount at `/sdcard`, GPIO pins (D0=3/CMD=1/CLK=2), `format_if_mount_failed`, 1-bit bus mode |

---

## Template

New pages: copy [_TEMPLATE.md](./_TEMPLATE.md) and follow the structure: What / How / Findings / Sources / Open questions / See also.

---

## Quick-reference gotcha index

The most expensive lessons learned, with page links:

- **Double-deref crash** on `esp_board_manager_get_device_handle` → [board-manager.md](./board-manager.md)
- **`esp-bmgr-assist` must be pinned to `==0.5.0`** → [build-toolchain.md](./build-toolchain.md)
- **`idf-component-manager` must be pinned to `==2.4.10`** → [build-toolchain.md](./build-toolchain.md)
- **Delete `sdkconfig` before enabling a new board device** → [build-toolchain.md](./build-toolchain.md)
- **`llm_profile` is a protocol enum** — setting it to a vendor name causes a boot loop → [llm-config.md](./llm-config.md)
- **NVS recovery: `esptool.py erase_region 0x9000 0x6000`** → [llm-config.md](./llm-config.md)
- **`gfx_img_set_src` with a runtime buffer spirals on hardware** — no canvas widget → [display-emote-gfx.md](./display-emote-gfx.md)
- **Missing EAF → silent blank face** (no build error) → [asset-pipeline.md](./asset-pipeline.md)
- **New NVS config field requires three file edits** → [jarvismcp-bridge.md](./jarvismcp-bridge.md)
- **`thinkingBudget` is deprecated** — use `thinkingLevel` → [gemini-live-api.md](./gemini-live-api.md)
- **Choppy Gemini audio** — single `rx_buf` overwritten; fix: PSRAM frame queue depth 128 → [audio-es8311-es7210.md](./audio-es8311-es7210.md)
- **Flat gain hard-clips speech** — use soft-knee limiter → [audio-es8311-es7210.md](./audio-es8311-es7210.md)
- **Display snapshots are software mirrors, not panel readback** — `/api/display/snapshot.*` only
