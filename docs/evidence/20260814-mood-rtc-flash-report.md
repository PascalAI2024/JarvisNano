# 2026-08-14 — Phase 5 moods + PCF85063 RTC: on-device flash report

Image flashed: `jarvisrobot_v5.bin` (1,673,808 B), app version
`voice-known-good-26-56-g3aff8e0`, ESP-IDF v5.5.4, flash mode DIO.
All four regions hash-verified by esptool. Source under test is **uncommitted**
working-tree work on branch `v5` (new `components/jr_rtc/`,
`components/jr_core/src/mood.c`, `host/test_mood.c`, plus edits to `main/main.c`,
`components/jr_display/`).

## Verdict

The feature works, and it shipped four defects — one of which made the device
unwakeable. All four are fixed and re-verified on hardware; see "Fixes applied".

**This work is UNCOMMITTED and partly UNTRACKED** (`components/jr_rtc/`,
`components/jr_core/{include/jr_core/mood.h,src/mood.c}`, `host/test_mood.c`).
`git stash` and `git reset --hard` both destroy untracked files, so this should
be committed before anyone runs either. Everything below is verified against the
image currently on the board.

## What ran, and what it showed (first flash, grok's tree as handed over)

| Check | Result |
| --- | --- |
| Flash + verify (4 regions) | PASS — all hashes verified |
| Boot to `boot complete` | PASS — 14.4 s, no error before the app is up |
| Host test suite (`host/`) | PASS — **117 tests, 0 failures** (`ctest`), incl. grok's `test_mood.c` |
| PCF85063 detected | PASS — `jr_rtc: PCF85063 online (CTRL1=0x00)` at 1059 ms |
| RTC **write** path (OS → chip) | PASS — `RTC seeded from wall clock` |
| RTC **read** path (chip → OS) | PASS — closed on the final boot, see below |
| Gemini WS + header auth | PASS — `x-goog-api-key header ACCEPTED`, phase → Listening |
| Cockpit JSON new fields | PASS — parses clean; `mood`, `brightness`, `rtc` all render |
| Mood ladder, full traverse | PASS — AWAKE → AMBIENT → WHISPER → DREAM |
| Voice disarm on rest | PASS — `armed=false` from WHISPER down |
| **Wake from rest by tap** | **FAIL** — deadlocked; device could not be woken |
| Stability | **FAIL** — SPI bus-lock assert, crash + core dump |

Note on "boot": no true power-on reset happened this session. Both resets were
`rst:0xc (RTC_SW_CPU_RST)`, which preserves the RTC domain — that is exactly why
the RTC read path stayed untested.

Measured ladder traverse (cockpit poll, device untouched):

```
uptime=249s mood=WHISPER br=22 phase=Idle armed=false
uptime=309s mood=DREAM   br=8  phase=Idle armed=false
uptime=370s mood=DREAM   br=8  phase=Idle armed=false
```

Brightness targets match `mood.c` exactly: 100 / 48 / 22 / 8.

## Defect 0 — the device could not be woken (blocking, user-visible)

This is the one that reads as "it's not working at all, nothing happens."

The rest ladder disarms voice by posting `VOICE_CONTROL_DISARM`, and that
handler sets `s_voice_privacy_paused = true` (`main.c:4699`). Tap-to-wake was
then gated on `!s_voice_privacy_paused` (`main.c:4916`) — a condition the
ladder's own disarm had just made false. So the wake path could never fire.

The second re-arm path did not save it either: `jr_mood_poke_awake()` sets the
mood to AWAKE directly, so the next `jr_mood_step()` reports `changed == false`
and the re-arm inside `if (mout.changed)` (`main.c:4623`) never runs.

Both routes out of rest were therefore closed. Once the device reached WHISPER
it stayed dark (22 %, then 8 %), deaf (`voice_armed=false`), and unresponsive to
touch until a reboot. Captured live with 21 registered touch events:

```
mood=DREAM brightness=8 phase=Idle
voice_armed=false privacy_paused=true ws_connected=false
touch: {"events":21,"last":{"kind":"tap","x":269,"y":233}}
```

The taps were reaching the firmware. They just could not do anything. (The
`jr_hal_touch: input queue overflow; dropped=16` warnings in the first capture
are the same story — a user tapping a deadlocked device faster than a wedged
consumer drained the queue. Not a touch-driver fault; don't chase it.)

## Defect 1 — SPI bus race crashes the device (blocking)

Observed twice — once live in this session, plus one earlier core dump already
in flash at boot. It is intermittent, not reliably reproducible on demand: a
~220 s soak and a 370 s uptime window both passed without it. The mechanism
below, not the hit rate, is what makes it real.

At 19,969 ms the device asserted and dumped core:

```
E bus_lock: spi_bus_lock_acquire_end(755): Cannot release a lock that hasn't been acquired.
assert failed: spi_device_release_bus spi_master.c:1413 (ret == ESP_OK)
```

Backtrace decoded against `build/jarvisrobot_v5.elf`:

```
spi_device_release_bus      spi_master.c:1413
panel_io_spi_tx_color       esp_lcd_panel_io_spi.c:404
tx_color / panel_co5300_draw_bitmap  esp_lcd_co5300_spi.c:164, :292
esp_lcd_panel_draw_bitmap   esp_lcd_panel_ops.c:37
panel_flush                 components/jr_display/src/jr_display.c:1367
gfx_render_part_area        gfx_render.c:149
gfx_render_dirty_areas      gfx_render.c:173/187
gfx_render_loop_task        gfx_core.c:201
```

**Root cause.** The crashing task is the `esp_emote_gfx` render loop, mid-flush.
`main.c:4607` calls `jr_display_set_brightness()` from the **voice task**;
that lands in `esp_lcd_panel_co5300_set_brightness()`, which issues its own
transaction on the same CO5300 QSPI device with no mutex against the flush.
The two collide and break the bus acquire/release pairing. The serial log
captures the collision literally — the `bus_lock` error and the brightness
INFO line are interleaved character-by-character in one line.

Corroborating: a core dump from an earlier crash was already present in flash
at `0xff0000` on the first boot of this session, so it is not a one-off.

The fix is ownership, not a retry: brightness must be issued from the display
task (or under the same lock the flush takes), never from the voice task.

## Defect 2 — the brightness write is unconditional (amplifies defect 1)

`main.c:4607` writes brightness every voice-task tick whether or not the value
changed. Measured over a 99.9 s untouched window:

- **889 panel writes, all `set brightness to 100%`, all identical — 8.9/sec.**
- 889 of 891 serial lines in that window were this one message. The console is
  unusable for debugging anything else.

Each write is a race window against the render task, so this is what turns
defect 1 from theoretical into observed. Fix: track last-written value and
short-circuit. The ramp itself is intentional and should stay; only the
steady-state repeat is wrong.

## Defect 3 — boot starts in AMBIENT, not AWAKE (cosmetic)

`app_main` calls `jr_mood_reset(&s_mood, 0)`, seeding `still_since_ms = 0`.
The first `jr_mood_step` runs at `now_ms ≈ 14,300`, so `still = 14.3 s`, already
past `JR_MOOD_AMBIENT_MS` (8 s). The device therefore dims to 48 % about 10 ms
after `boot complete`:

```
I (14379) jarvis_v5: boot complete — always-ready voice requested
I (14389) jarvis_v5: mood -> AMBIENT brightness=48 voice=1
```

This contradicts `jr_mood_reset`'s own stated intent, which sets `JR_MOOD_AWAKE`.
Seed with the current tick instead of 0.

## Not a defect — worth knowing

`user_busy` includes `JR_ST_LISTENING && s_listen_speech_active`. In a room with
sustained noise (measured mic RMS 338 against a floor of 40) the session never
drains, so the device holds AWAKE indefinitely and never rests. Observed for
100+ s before the session drained. The ladder is correct; it is coupled to VAD
tuning, and that coupling should be a deliberate decision rather than a surprise.

## RTC read path — CLOSED (2026-08-15 00:05)

The final verification boot cleared the RTC domain, so `time()` was invalid and
`device_wall_time()` fell through to its second branch. `jr_rtc_get()` was
consulted for real and returned a correct time:

```
I (1052)  jr_rtc:     PCF85063 online (CTRL1=0x00)
I (14132) jarvis_v5:  wall clock 2026-08-15 00:05 (rtc_seed=1)
```

`rtc_seed=1` is set only inside the `jr_rtc_get() == ESP_OK && tm_year >= 2020`
branch, so this proves the BCD decode and register mapping in `jr_rtc.c`, not
just chip presence. The date and time were both correct. That was the last
untested piece of grok's feature, and the whole point of the component —
correct time on a cold start without SNTP — now holds on hardware.

The same capture verified the boot behaviour of the shipping image: 201 lines,
**1** brightness write (the init to 100 %), **0** asserts, **0** reboots, and the
device held AWAKE at full brightness rather than dimming itself at boot.

## Superseded — how this gap was originally left open

The point of `jr_rtc` is correct time after a cold boot with no Wi-Fi. Both
captured boots printed `rtc_seed=0`, meaning `device_wall_time()` returned on its
first branch and `jr_rtc_get()` was never called. Reset cause was
`rst:0xc (RTC_SW_CPU_RST)` — a software reset, which preserves the ESP32
RTC-domain clock, so `time()` was already valid and the fallback never ran.
esptool's RTS-driven reset did not produce a power-on reset on this board.

This is kept as a note on method, not an open item: a `RTC_SW_CPU_RST` is not a
cold boot, and reading `rtc_seed=0` proves nothing about the RTC — it only means
the OS clock was still valid. Watch for `rtc_seed=1` when testing this path.

## Fixes applied, and re-verified on hardware

| # | Fix | Where |
| --- | --- | --- |
| 0 | Tap-wake now gates on `s_flip_muted`, not `privacy_paused`, so rest can undo itself; clears the rest caption. `s_flip_muted` hoisted to file scope for that check. | `main.c` |
| 1 | Brightness moved OFF the caller's task. `jr_display_set_brightness()` now only publishes an atomic target; `brightness_pump()` applies it from `panel_flush`, on the render task, with the bus idle. | `jr_display.c`, `jr_display.h` |
| 2 | Same pump short-circuits unchanged values, which is what kills the write storm. | `jr_display.c` |
| 3 | `jr_mood_reset()` seeds from the current tick; the stillness clock is then restarted at `boot complete`, so the first AWAKE window begins when JARVIS is actually usable. | `main.c` |
| 4 | Rest thresholds retuned — AMBIENT 8 s → 20 s, WHISPER 33 s → 5 min, DREAM 93 s → 15 min. AMBIENT keeps voice armed, so it stays quick; the mic-off rungs move far out. | `mood.h` |

**Wake-from-rest, proven end to end.** The room was too noisy to let the device
rest passively (see "Not a defect" below), so this was forced with a throwaway
test image — thresholds cut to 3 s / 9 s / 20 s and `user_busy` pinned false, so
the ladder descends on a timer. The tap→re-arm branch under test is byte-identical
to the shipping build. Result:

```
wait-for-rest    up= 41s mood=DREAM    br=  8 armed=False priv=True  taps=0
>>> RESTED + voice DISARMED. firing sim tap <<<
tap -> (200, '{"queued":true,"x":233,"y":233}')
after-tap+2s     up= 43s mood=AWAKE    br=100 armed=True  priv=False taps=1
RESULT=WAKE_OK — tap re-armed voice out of rest
```

DREAM at 8 % with voice off → one tap → AWAKE, 100 %, armed, in under 2 s. The
real thresholds were then restored, rebuilt, reflashed, and re-verified.

Note for future sim-lane tests: `/api/input/tap` takes **query** params and
requires the `X-JarvisNano-Control: 1` header — a JSON body gets 403.

Measured on the rebuilt image, 55 s capture from boot:

| | grok's build | after fixes |
| --- | --- | --- |
| Panel brightness writes | 889 per 100 s (8.9/s, all identical) | **5 total** (the ramp, nothing else) |
| Serial lines, 45 s idle | ~400, almost all brightness spam | **1** |
| `spi_device_release_bus` asserts | 1 | **0** |
| Reboots | 1 | **0** |
| Wake from rest by tap | deadlocked | **works, <2 s** |
| Host tests | not run | **117 / 117 pass** |

The 5 remaining writes are exactly the intended fade — 92 → 84 → 76 → 52 → 48 %.
The ramp is preserved; only the redundant repeats are gone.

## Second pass — one hardening fix, and one phantom worth recording

Chasing a suspected regression ("could a tap silently cancel a deliberate
mute?") turned up a **stale-flag** problem but **no un-mute bug**:

- **Real and fixed.** `s_mood_rest_disarmed` was only ever cleared by the mood
  ladder itself, so an arm from the API, the shade or face-up left it set and
  the flag stopped describing reality. It is now cleared in the
  `VOICE_CONTROL_ARM` handler — the single choke point every arm passes through
  — and the ladder no longer claims a disarm it did not cause.
- **The phantom.** A tap *does* re-enable voice on a muted, idle device, and
  that is correct, pre-existing behaviour, not a defect: the tap-toggle block is
  unchanged from `HEAD`, and `main.c:4696` states the intent outright — "A tap
  is an actual power toggle for voice, not a mysterious half-commit button."
  The test that "failed" was asserting the opposite of the product's design.
  **Do not `fix` this**; doing so would break tap-to-talk.

Both behaviours were re-verified on a forced test image: tap still wakes a
mood-rest disarm (`RESULT_A=PASS`), and 117/117 host tests pass.

## Board skill audit (`AI_SKILLS`)

The repo ships exactly one board-related skill:
`managed_components/espressif__esp_board_manager/tools/AI_SKILLS/lcd_touch_i2c_migration/SKILL.md`.
It is **already satisfied** — `board_devices.yaml:95` uses `type: lcd_touch` +
`sub_type: i2c`, and the boot log shows the generic `DEV_LCD_TOUCH_SUB_I2C` path
initialising the CST9217. Nothing in it bears on the crash.

Two leftovers it would flag, both cosmetic:

- `components/gen_bmgr_codes/board_manager.defaults:17` still sets
  `CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT=y`, which the skill's validation
  step 4 says should be gone. It compiles the legacy driver in for nothing.
- `components/jr_hal/src/input_touch.c:181` mentions the legacy type in a comment.

## Reproduce

```bash
NO_BUILD=1 ./scripts/flash-v5.sh
python3 scripts/usb-monitor.py --port /dev/cu.usbmodem1101 --seconds 100
```

Backtrace decoding needs the toolchain from the build image; there is no local
ESP toolchain on this machine:

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.4 \
  xtensa-esp32s3-elf-addr2line -pfiaC -e build/jarvisrobot_v5.elf <addrs>
```
