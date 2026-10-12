# BUGLOG — owner testing, 2026-08-27 evening

> **Historical session record.** Statuses below are the evidence captured on
> 2026-08-27, not the current task board. Use [`PLAN.md`](PLAN.md) and
> [`docs/NEXT_SESSION.md`](docs/NEXT_SESSION.md) for live priorities.

Live log of every defect from the owner's hands-on session, with state and
evidence. Owner reports paraphrased; device evidence cited. Rows close only
when verified fixed ON HARDWARE.

| # | Symptom (owner) | Root cause / evidence | Status |
|---|---|---|---|
| B1 | Voice choppy, not fluid | Feeder starved (prio 5 + PSRAM stack): 420 ms acoustic holes vs clean electrical taps | FIXED `11e66a8`, on device |
| B2 | Crackle/distortion | 4x playback gain railed DAC (902/48000 samples at rail) | FIXED (2.5x default), on device |
| B3 | "No sound at all" | Stuck fast-kill DAC mute; no UNMUTE successor; state invisible | FIXED `482c81d` (arm force-clears; dac_muted on API), on device |
| B4 | Doesn't stop when talked over | Speak-mic 9 dB → owner read ~20 rms; gate 0.30 tuned to wrong hardware | FIXED `4c3a837` (21 dB + gate 0.10 + server HIGH sensitivity); **owner session shows barge_events=3 — latching** |
| B5 | Device goes silently deaf ("no response") | Tap = privacy mute (trap); stuck challenge flag; no re-arm coverage | CLOSED `dd5d48b`: tap can never mute (stop-talking/attention only); challenge teardown + re-arm + wake net all on device |
| B6 | Wake didn't respond | Was correct behavior: device was tap-muted (B5); wake honors deliberate mutes | CLOSED with B5; wake verified with bloom on hardware |
| B7 | Watch↔face transition "not smooth and weird" | Clock overlay pops (instant full dim, no easing) | CLOSED `a615a52`: watch/canvas/caption/brightness all ease — captured mid-crossfade, no banding |
| B8 | Swipes/double-tap do nothing | Never implemented | CLOSED `dd5d48b`+`db277ef`: glance verified on glass (caption in capture), double-tap bloom wired |
| B9 | Unexplained reboot (~16:40) | Coredump blank = no panic; suspect USB brownout at speaker+Wi-Fi peak or replug | WATCH — serial `rst:` banner will identify next occurrence |
| B10 | Session drops every few minutes (StaleDeadline) | Gemini Live ~10-min connection cap is documented lifecycle; reconnect+resumption already wired; deaf-after-death fixed | MITIGATED; long-session soak pending |
| B11 | PSRAM sag during heavy testing (1.6 MB free) | Canvas + clips + session buffers; no failure observed | WATCH |
| B12 | Answers ambient room noise | START_SENSITIVITY_HIGH (my tuning) made server VAD trigger-happy | CLOSED `db277ef`: default sensitivity; local gate carries barge |
| B13 | “Only swipe-down works” | Hardware log captured all four directions; horizontal actions were suppressed while shade was open and several successful actions lacked unmistakable feedback | VERIFIED ON HARDWARE: left/right/up/down each produced the intended action; horizontal swipe closed the shade first |
| B14 | Hold cannot mute reliably | Hold required 1200 ms with less than 18 px drift; natural holds became taps/swipes | VERIFIED 10/10 ON HARDWARE; persistent gold privacy ring captured |
| B15 | 128 KB log dominated by WebSocket errors | Two-frame batches + separate TX lock did not survive a real conversation | REGRESSED: at ~15 min `tx_would_block=1620`, `tx_drops=449`, deaths/reconnects=3; transport is next P0 |
| B16 | Wi-Fi OTA rejected at image validation | Uploaded ota_1 prefix was byte-identical to host; ESP32-S3 rejected an executable slot starting at `0x1000000` with bogus eFuse revision `v292.95` | FIXED IN TABLE: ota_1 moved below the 16 MB app-mapping boundary; data-only storage moved above |
| B17 | Speech intermittently hiccups | Local barge falsely triggered on AEC echo, forcing Speaking→Listening→Speaking; live controlled turn reproduced it | FIXED LIVE via `barge=0`; baked default now server-VAD interruption with local barge opt-in |
| B18 | “Use the device as your tool; double-tap returns normal Jarvis” | Operator lease existed, but ordinary taps reclaimed it and Desk pairing stored an empty Keychain password | VERIFIED ON HARDWARE: native Keychain pairing persists, interactive card action returned, violet Codex mode survives single taps, double-tap and TTL restore Listening |
| B19 | Volume too low and neither owner nor Jarvis could clearly adjust it | Remote gain desynchronized persisted swipe state; caption was scale-1 and unreadable; Gemini lacked a local level tool | VERIFIED: paired/physical/Gemini paths share persisted volume, live `set_volume(70)` succeeded, output restored to 90, scale-2 receipt captured |
| B20 | Voice not fluid and native Gemini duplex never interrupts | Firmware was explicitly in manual VAD: first 8 onset frames (~256 ms) were discarded and uplink stopped during model speech, while local barge was disabled | FIXED LOCALLY: restored Gemini server VAD on queue-safe two-frame/64 ms batches and 20 ms send wait; hardware flash + 10-minute duplex soak pending |

Live capture: 5-minute serial log running during owner testing
(`owner-test-serial.log` in session scratchpad); counters snapshotted before
and after. Device state is queryable any time: `scripts/jarvisctl.py status`.

## t107 deployed candidate, acceptance pending — 2026-10-11

This is the earlier checkpoint; voice was subsequently accepted on owner
observation (2026-10-11, tested image unspecified). See Addendum A below.

Independent review found retained half-batch PCM across privacy/transport gaps,
automatic diagnostic clearing that reset the absolute companion cursor, and
missing companion privacy revalidation after copying PCM. All three are
repaired in the local candidate, with executable sequence regressions and
production wiring checks. Final host suites pass 11,451 / 854 / 166 / 81 / 38.

The candidate also gates muted audio power, adds the guarded N8.12 quiet clock,
and applies rest/network guards to weather's first and retry fetches. The owner
removed the interrupted generated build directory, and the explicitly authorized
ESP-IDF 5.5.4 rebuild passed with 55.786% app-partition headroom. The local
candidate and proof were committed before OTA (`0b4108f`). After owner wake,
the parent restored unexpectedly unmuted privacy before the paired upload.
Candidate `ota_1` passed probation to valid with authenticated health, zero
flush errors and mic/ADC/DAC/PA reported off. Live idle/WATCH/DREAM captures were
reviewed; DREAM shows both the no-time fallback and valid 16:57 digits. Matched
idle/WATCH windows and timer sleep completed with privacy muted; timer wake
returned after 317.484 seconds. Voltage rebound and differing charge levels
leave battery-life improvement unproven. Listening-state power is excluded;
voice remains owner check pending, ready for physical unmute. The fixes are
not closed solely by software telemetry.
N7.21 remains blocked on physical PKEY event resolution. See
[the t107 report](docs/evidence/t107-power-baseline.md).

## t107 Addendum A — 2026-10-11

| Defect | Evidence | Status |
|---|---|---|
| DETAIL DOWN advances after closing | Live WATCH sheet followed by DOWN showed WEATHER; production routing called `nav_next` unconditionally. | Fixed in `7578238`; actual routing tests/mutation pass; post-OTA mirror stays on WATCH. Physical gesture proof remains separate. |
| Detail dismissal shows a controls exit hint | Live WEATHER outside-sheet tap showed `CONTROLS - UP TO CLOSE`. | Fixed in `7578238`; panel caption tests/mutation pass; post-OTA WEATHER mirror has the correct caption. |
| Rest cadence stretches finite UI transitions | Actual fade tick discarded elapsed time above 100 ms. | Fixed in `7578238`; production 50/200/500 ms fake-clock regression and mutation pass; live endpoints captured. CLI latency prevents physical duration proof. |
| Dial cycling exhausts snapshot headroom | The muted sweep reached HTTP 503 after loading FUTURE/JARVIS/DIVER; free PSRAM was below one snapshot allocation. | Fixed in `7578238`; ownership/failure regressions pass; 18 live style captures across three cycles succeed without cache reset or asset/flush errors. |
| Failed presenter locks consume refresh keys | DREAM keys, snapshot and test-pattern acknowledgement advanced without a submitted refresh. | Fixed in `7578238`; retained-key injected-fault tests/mutations and combined gate pass. No live fault injection. |

Dead excluded tilt code and a duplicate byte-order helper were removed without
claiming a runtime gain. Battery acceptance is unproven. The after power gate failed timer
wake; its sleep-end/DREAM proof is missing. The owner subsequently woke it
by hand and authenticated readback succeeds. Listening-state power is excluded. Voice acceptance is owner-observed
(reply `yes`, 2026-10-11); its tested image was not supplied. The last authenticated
readback verified privacy muted, as does the later manual recovery readback.
Follow-up authenticated health/cockpit/sleep retries at 00:10–00:11Z on
2026-10-12 also timed out despite reported ping replies. Wake/API behavior and
the larger awake drain fits remain possible regressions versus baseline;
different SoC/rebound are confounds, not an established explanation.
Manual recovery readback at 00:15:40Z shows uptime 231,756 ms, wake `touch`,
RTC sleep count 3, OTA valid and privacy muted/audio off. Wi-Fi comes up in
5,037 ms on this boot. Reset reason is not exposed; the PSRAM ring covers only
this boot and RTC holds no timed wake history. Count 3 permits an intervening
wake/resleep inference but cannot identify when or why. Timer/API acceptance
and battery regressions remain open; manual recovery cannot close them.
See [Addendum A](docs/evidence/t107-addendum-a.md).

## t107 B diagnosis — 2026-10-12

The deployed sensor API does not expose charge-limit registers or measured
net charge current. A fixed sampler-only AXP2101 cache is added for paired
diagnosis: per-register errors, attempt age and last successful-read age are
retained; failed or stale values cannot decode as current configuration.
HTTP reads perform no PMIC transaction. Charge and rail registers are not
written by this diagnostic. Live image `03864cc` is valid and muted after
the charged A/B: 200mA constant-charge limit, 4.2V target, three complete
scans. Its 300-second timer wake passed at a 305.937-second API upper bound.
Retain 200mA until the cell rating is verified; no PMIC setting was changed.
See [B: CHARGE](docs/evidence/t107-charge-diagnostic.md).

Display startup previously had no first-transfer timing. The diagnostic
records init start/end and the first successful color-transfer callback,
including early callbacks and failed init. It is a boot software proxy;
physical first-light and a complete Glance remain unmeasured.
