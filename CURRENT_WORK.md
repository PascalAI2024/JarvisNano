# CURRENT WORK — ZeroChat companion protocol

**Updated:** 2026-09-13  
**Owner:** Main  
**Jarvis work item:** `jarvisnano-desk / zerochat-jarvisnano-companion-20260913`  
**Checkout:** `C:/Users/pasca/dev/projects/JarvisNano`, branch `main`, local uncommitted implementation; not flashed, deployed, or accepted on physical hardware.

## Objective

Keep native Gemini Live as JarvisNano's standalone default while allowing an explicitly paired ZeroChat phone to become the optional private-Wi-Fi brain and voice path. BLE remains a later transport phase.

## Implemented

- `jr_audio_diag_copy_since` exposes bounded 16 kHz PCM windows using an absolute end-exclusive cursor and reports ring-buffer overruns without changing the codec owner.
- The operator lease now has explicit `codex` and `zerochat` owners. Strict token pairing and physical control authority apply to ZeroChat even when development diagnostics are open. ZeroChat renewal and release require the random current lease identifier under the ownership lock; stale renewal cannot create a fresh lease and mismatched owners or IDs cannot release one another.
- `GET /api/companion/audio/in` requires strict pairing, a live ZeroChat lease identifier, and unmuted privacy. It returns cursor/drop headers and raw mono PCM16LE at 16 kHz.
- `POST /api/companion/audio/out` requires strict pairing, a live ZeroChat lease identifier, and control permission. It accepts bounded sequenced mono PCM16LE at 24 kHz, reports partial acceptance and playback state, treats only the immediately previous sequence as an idempotent retry, supports sequence rollover, accepts an empty end marker for drain polling, revalidates ownership under the enqueue lock, and rejects uploads exceeding a 12-second absolute receive deadline.
- Cockpit/brain state reports `private_wifi` only while ZeroChat owns a live lease; otherwise it remains `cloud_gemini`. Physical captions identify the current ZeroChat or Codex owner. Every protected control diagnostic requires token authentication plus its control-intent marker; only the physical-code pairing claim is unpaired.
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) records the pairing, lease, cursor, PCM, partial-write, drain, and rollover contracts.

## Verification

- Preflight synchronization preserved every tracked and untracked local change through a temporary stash, fetched `origin/main`, and confirmed `main` was already current at `3760ce6b6455bba4f4bbb7ecfc243e545c37ae23`; the stash reapplied without conflicts and was removed.
- `scripts/host-tests.sh`: all five suites passed — display HUD **11,451 checks**, shell **725 checks**, host core **137 tests**, tool templates **81 checks**, desk Python **37 tests**.
- `scripts/build-v5.sh`: ESP-IDF 5.5.4 build passed after visibly recompiling `http_routes.c` and `main.c`; `build/jarvisrobot_v5.bin` is `0x1c1ff0` bytes with 56% of the smallest app partition free.
- Live trusted-LAN OTA passed on the physical 1.75C at `192.168.50.221`: the previous firmware's physical BOOT window issued a one-time legacy token, `/api/ota/upload` accepted all **1,843,184 bytes** and returned HTTP 200 with `rebooting:true`, and the rebuilt device returned authenticated health with OTA `running=ota_0`, `boot=ota_0`, `state=valid`, `last_error=ESP_OK`, and no invalid image.
- ZeroChat's focused integration test exercises the protocol against a real local mock HTTP device through pair → cockpit → lease → lease-ID renewal → mic pull → speaker push → lease-ID release. Independent final behavior and security re-reviews found no remaining blocker.

## Verification limits

- The firmware is flashed and its physical BOOT pairing gesture, trusted-LAN upload, reboot, and OTA probation-to-valid path are now observed.
- No physical microphone capture, audible speaker playback, privacy denial, operator conflict, partial playback, Wi-Fi loss, or end-to-end ZeroChat turn has been observed yet.
- The previous image used the legacy code-free claim during this migration. The installed image now requires the visible random six-digit code for future claims. The one-time token remained process-local and was not written to the repository or handoff.

## Security boundary

The companion protocol intentionally carries PCM and its bearer token over plaintext HTTP on the user's trusted LAN. Physical BOOT pairing, strict bearer checks, a single live operator lease, privacy mute, and control ownership are the current boundary. Do not expose the device API across a routed or untrusted network. Existing NVS-at-rest limitations still apply.

## Desert Ant decision

No Desert Ant code was added. It is a phone-side model suite, not a replacement for pairing, transport, or the ZeroChat reasoning path. Its `Clear` denoiser can be reconsidered later as an explicit opt-in experiment; current sample-rate/native-integration costs and the license's attribution/telemetry obligations make it inappropriate for the core no-telemetry path now.

## Next acceptance

1. Pair a physical Android 13+ phone through the installed firmware's visible six-digit BOOT window.
2. Prove one complete audible turn, privacy denial, operator conflict, partial speaker writes, Wi-Fi loss, and lease expiry/recovery.
3. Repeat on an iPhone after compiling ZeroChat on macOS.
4. Add BLE only after Wi-Fi behavior is accepted, retaining the same authority and sequencing invariants.
