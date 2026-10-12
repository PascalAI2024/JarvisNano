# Current work â€” t107 Addendum A, power gate blocked

**Updated:** 2026-10-12 UTC

Source `7578238` on `t107/codex` repairs DETAIL close/navigation and dismissal
captions, finite fades, baked dial source lifetime and presenter refresh retry.
Dead tilt math and duplicate byte-order code are removed. Bounded battery-rim
traversal keeps all 28,566 compared frames identical; 21 paired host timings
improve. Production source removes 133 lines and adds 160.

All six host suites pass (11451/2200/109/166/81/38), behavior mutations fail
as expected, and the ESP-IDF build leaves 55.769% app headroom. Paired OTA
reached `ota_0 state=valid` at 59,727 ms; 32 panel captures and three complete
style cycles succeeded without asset-load/flush errors.

Owner voice acceptance is recorded (reply `yes`, 2026-10-11, audible answer;
owner-observed, tested image unspecified). Privacy was already muted on receipt
and remained muted in every subsequent authenticated check and power sample.
Listening-state power remains excluded.

**BLOCKED: gate `after-long` failed: `timer wake failed`.** Both 720-second
awake windows completed, but the after 1800-second sleep did not become
reachable within the wake retry period. Sleep-end/DREAM evidence is missing;
cleanup and final health also timed out. Last authenticated boundary: valid,
muted, all audio off, no lease, flush errors zero, internal block 30,720 bytes.
The owner subsequently woke it by hand. No USB flash or privacy bypass is attempted.

Voltage drain fits were larger after OTA (idle 29.42â†’100.20, WATCH
65.83â†’126.56 mV/h), with different SoC and rebound. Battery savings remain
unproven. Long WATCH software rendering was 45.438â†’43.790 ms/frame; the
short profile was slower, 37.154â†’46.078. Both results are retained.

At 00:10:37â€“00:11:01Z on 2026-10-12, authenticated health, cockpit and sleep
GET retries all timed out despite orchestrator-reported ping replies. Wake
timing/reason and current privacy were unreadable at that retry. Compared with the
successful baseline cycle, this is an acceptance regression and a possible
firmware wake/API regression. The worse drain fits are a possible battery
regression; different SoC/rebound are observed confounds, not a proven cause.
Audio/sleep/network source is unchanged; DREAM presenter refresh retry did
change. The report records the bounded source audit and failed readback receipt.

After the 00:12Z owner update, authenticated reads at 00:15:40Z succeed:
uptime 231,756 ms (estimated boot 00:11:49Z), wake `touch`, sleep counter 3,
`ota_0 state=valid`, privacy muted, no capture, mic/ADC/DAC/PA off, zero flush
errors and 31,744-byte internal headroom. This is manual recovery, not timer
acceptance. Current-boot Wi-Fi gets an IP at 5,037 ms. Reset reason is not
exposed; the PSRAM log is lost on reboot and RTC has no timestamped wake
journal. Count 3 is consistent with an intervening wake/resleep, but cannot
date it or establish its cause. The original sleep-end/DREAM data remain absent.
No device writes or extra measurement runs. Historical boot/network evidence
is needed to distinguish late timer from late API or another wake.

See [the Addendum A report](docs/evidence/t107-addendum-a.md) for error bounds,
receipts, capture links and the three waves with three unique executors.

## Previous work â€” t107 OTA valid, retained checkpoint

The voice-pending text below is the earlier checkpoint. Owner acceptance
arrived later (2026-10-11, reply `yes`, tested image unspecified); the current
Addendum A report records it and verifies mute restored.

**Updated:** 2026-10-11

Candidate on `t107/codex`: privacy-muted ADC/DAC/PA shutdown,
paced silent text input, guarded near-black DREAM minute clock (N8.12),
rest-aware first/retry weather fetches, and privacy batch/cursor/export repairs.
The existing N8 battery/rim/charging/heading fixes were already present.
N7.21 remains blocked on physical PKEY event resolution.

Before measurements on battery completed with privacy muted. Final host suites
passed 11,451 / 854 / 166 / 81 / 38, with 54 scratch mutants rejected. After the
owner removed the incomplete generated `build/`, the authorized ESP-IDF 5.5.4
`RECONFIGURE=1` build passed on the unchanged tested source. The image is
1,854,464 bytes, with 2,339,840 bytes (55.786%) free in the smallest 4 MiB app
partition; SHA-256 is recorded in the report. The local candidate and proof are
committed before OTA as requested (`0b4108f`). After the owner wake, the initial
health read unexpectedly showed privacy false; the parent restored mute with
`armed=0` and verified it before upload. The paired upload succeeded, and
`ota_1` passed probation to valid at 57,495 ms. Authenticated health shows zero
flush errors, a 31,744-byte largest internal block and mic/ADC/DAC/PA off.
Matched 300-second idle/WATCH windows and timer sleep are complete. Timer wake
returned after 317.484 seconds with privacy muted. Live idle/WATCH/DREAM captures
were opened; DREAM renders both the no-time `--:--` fallback and a valid `16:57`
clock. Voltage rebound and different charge levels confound the lower apparent
drain fits: measured battery-life improvement remains unproven.
Listening-state power measurement is excluded by the orchestrator. Voice is
**owner check pending**, ready now; stop for a physical unmute
and one spoken turn, then restore mute.

The successful candidate authenticated readback confirmed privacy muted,
inactive lease and running OTA state `valid`. See the
[t107 report](docs/evidence/t107-power-baseline.md) and its retained evidence.
Four waves used seven unique subagents; the parent alone operates the device.

## Previous work â€” live display documentation

**Updated:** 2026-10-02

Added current privacy-muted face and transient digital watch captures, sampled
clips, original pixels, and timestamped metadata. The existing paired diagnostic
client captured over LAN; USB and the running binary's immutable revision were
not established. Public source baseline: `63c30a8`. No firmware or configuration
change was performed. Existing uncommitted development work remains separate.

See [the capture record](docs/evidence/20261002-live/README.md) for evidence and
limits. The earlier companion implementation record is retained below.

## Previous work â€” ZeroChat companion protocol

**Updated:** 2026-09-13  
**Owner:** Main  
**Jarvis work item:** `jarvisnano-desk / zerochat-jarvisnano-companion-20260913`  
**Checkout:** `<local-workspace-path>`, branch `main`, local uncommitted implementation; not flashed, deployed, or accepted on physical hardware.

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
- `scripts/host-tests.sh`: all five suites passed â€” display HUD **11,451 checks**, shell **725 checks**, host core **137 tests**, tool templates **81 checks**, desk Python **37 tests**.
- `scripts/build-v5.sh`: ESP-IDF 5.5.4 build passed after visibly recompiling `http_routes.c` and `main.c`; `build/jarvisrobot_v5.bin` is `0x1c1ff0` bytes with 56% of the smallest app partition free.
- Live trusted-LAN OTA passed on the physical 1.75C at `<device-ip>`: the previous firmware's physical BOOT window issued a one-time legacy token, `/api/ota/upload` accepted all **1,843,184 bytes** and returned HTTP 200 with `rebooting:true`, and the rebuilt device returned authenticated health with OTA `running=ota_0`, `boot=ota_0`, `state=valid`, `last_error=ESP_OK`, and no invalid image.
- ZeroChat's focused integration test exercises the protocol against a real local mock HTTP device through pair â†’ cockpit â†’ lease â†’ lease-ID renewal â†’ mic pull â†’ speaker push â†’ lease-ID release. Independent final behavior and security re-reviews found no remaining blocker.

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
