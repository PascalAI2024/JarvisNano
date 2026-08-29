# JarvisRobot Reference Knowledge Base

Organized reference pages for JarvisNano’s primary 1.75C firmware and labeled compatibility tracks. Each page covers one system area: what it is, how it is used, dated findings, primary sources, and open questions.

**Capture rule:** After any research pass, record findings in the matching page below using `_TEMPLATE.md`. Cite primary sources (GitHub URL or `file:line`). Never put secrets, API keys, internal URLs, Wi-Fi credentials, device MACs/IPs, or bearer tokens in reference pages or anywhere in this repo.

---

## Pages

| Page | What it covers |
|------|---------------|
| [board-manager.md](./board-manager.md) | `esp_board_manager` device handle API, the double-deref gotcha, generated code files |
| [build-toolchain.md](./build-toolchain.md) | IDF v5.5.4 Docker image, `idf-component-manager==2.4.10` pin, `esp-bmgr-assist==0.5.0` pin, sdkconfig regeneration. Live recipe is `docs/BUILD.md` (`build-v5.sh`). |
| [waveshare-amoled-175.md](./waveshare-amoled-175.md) | Original 16 MB board compatibility reference |
| [board-175c.md](./board-175c.md) | **Current 1.75C delta:** 32 MB DIO, revised reset/MCLK pins, removed expander/RTC/SD, live PKEY, and safe provisioning boundary |
| [audio-es8311-es7210.md](./audio-es8311-es7210.md) | Codec chain, `esp_codec_dev` volume curve (0 dB ceiling), soft-knee limiter, choppy-audio PSRAM frame queue, 16kHz/24kHz I/O rates |
| [display-emote-gfx.md](./display-emote-gfx.md) | `esp_emote_gfx` widget API, proven absence of `gfx_canvas`, why runtime CPU-drawn buffers produce a spiral, sanctioned reactive-face paths |
| [gemini-live-api-v5.md](./gemini-live-api-v5.md) | Current v5 transport, model, session, and protocol verification |
| [gemini-live-api.md](./gemini-live-api.md) | Historical v4 observations; superseded for current decisions |
| [aec-barge-in.md](./aec-barge-in.md) | Historical AEC/barge design rationale; current ownership is `components/jr_audio` |
| [jarvismcp-bridge.md](./jarvismcp-bridge.md) | Typed `/device/v1/invoke` contract, legacy fixed-template compatibility, secret provisioning, and fail-closed policy |
| [llm-config.md](./llm-config.md) | `llm_profile` protocol enum (not a vendor name), boot-loop recovery via `esptool erase-region`, full config field table |
| [asset-pipeline.md](./asset-pipeline.md) | EAF/AAF format, `gen_mascot_eaf.py` local encoder, `esp_mmap_assets` packer, silent `"lack":true` blank face, `emote_assets.bin` build flow |
| [sdmmc-storage.md](./sdmmc-storage.md) | Original-1.75 SDMMC compatibility reference; not 1.75C hardware |

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
- **Gemini model/session behavior** — use [gemini-live-api-v5.md](./gemini-live-api-v5.md)
- **Choppy Gemini audio** — single `rx_buf` overwritten; fix: PSRAM frame queue depth 128 → [audio-es8311-es7210.md](./audio-es8311-es7210.md)
- **Flat gain hard-clips speech** — use soft-knee limiter → [audio-es8311-es7210.md](./audio-es8311-es7210.md)
- **Display snapshots are software mirrors, not panel readback** — `/api/display/snapshot.*` only
