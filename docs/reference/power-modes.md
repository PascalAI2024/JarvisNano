# Power modes on the 1.75C — what the chip offers, what the firmware uses

**Date:** 2026-09-01. **Status:** current; supersedes the "CPU frequency, light
sleep, and PMIC rails remain blocked" line that `docs/HARDWARE.md` carried
until this date.

## What

The ESP32-S3 has four power modes. This page records which ones JarvisNano
drives, how, and what was measured on the way, so the next person does not
rediscover the two gotchas at the bottom.

| Mode | Used | How |
|---|---|---|
| Active, in gears | yes | `CONFIG_PM_ENABLE` with **max pinned to min** per gear (`cpu_gear_set`, `main/main.c`): 240 MHz whenever anything is happening (voice armed, an update, a lease, USB present, the audio self-test), 160 MHz at rest on the cell once the session is closed. No dynamic scaling, no `PM_DFS_INIT_AUTO`, no tickless idle. `/api/debug/gain?cpu=160` forces a gear for a bench; `cpu=0` returns to auto |
| Modem-sleep | yes | `jr_net_set_power_save()` toggles `WIFI_PS_MIN_MODEM` from the mood ladder: realtime while voice is armed, an update is in flight, or a companion holds the device; saving otherwise (`main/main.c`, the mood tick) |
| Light-sleep | no | Automatic light sleep cannot engage: the I²S capture that keeps WakeNet listening holds the APB clock lock the whole time the device is awake. DFS on its own is unmeasurable here — the AXP2101 exposes no current reading (`jr_power_t` has voltage and percent only) |
| Deep-sleep | yes | `enter_deep_sleep()` in `main/main.c`, ten minutes into DREAM on battery |

**Battery saver.** Below 20 % on the cell every wait on the ladder is divided
by four (`JR_MOOD_SAVER_DIV`): AMBIENT at 5 s, WHISPER at 75 s, DREAM at
3 min 45 s, deep sleep 2 min 30 s later. It is an input to the pure ladder,
so plugging in restores the normal waits at once. The speaker's pre-roll is
adaptive too (`components/jr_audio`): 600 ms, +300 after any reply with a
hole, −200 after three clean replies, between 600 and 1500.

STATUS reports the gear and the modem state on its `CPU` row (`240 LIVE` /
`160 SAVE`) and
the die temperature on `CHIP` (the S3's own thermometer, `esp_driver_tsens`;
43 °C at rest on the cell, `RUNNING HOT` headline from 70 °C).

## How the deep sleep works

```mermaid
stateDiagram-v2
    [*] --> AWAKE
    AWAKE --> AMBIENT: still 20 s
    AMBIENT --> WHISPER: still 5 min
    WHISPER --> DREAM: still 15 min
    AWAKE --> DREAM: face-down
    DREAM --> SLEEP: 10 min more · on battery · no update · no companion · 3 min since boot
    SLEEP --> AWAKE: lift (QMI8658 INT1, GPIO21)
    SLEEP --> AWAKE: touch (CST9217 INT, GPIO11)
    SLEEP --> DREAM: 4 h timer
    AMBIENT --> AWAKE: motion / voice
    WHISPER --> AWAKE: motion / voice
    DREAM --> AWAKE: motion / tap / wake word
```

- **The rule is pure.** `jr_mood_sleep_due()` (`components/jr_core`) is true
  once the ladder has sat in DREAM for `JR_MOOD_SLEEP_MS` (10 min). The
  composition root adds the world: never on USB (USB holds the ladder AWAKE
  anyway), never while `s_ota_active`, never under an operator lease, never in
  the first `SLEEP_MIN_UPTIME_MS` (3 min) after any boot.
- **The hand-off, in order:** caption `SLEEPING - LIFT TO WAKE`; the IMU
  sampler stops; `jr_imu_arm_wake_on_motion(100)` programs the QMI8658
  Wake-on-Motion engine onto INT1 (recipe in
  [imu-interrupt-routing.md](./imu-interrupt-routing.md)); the render task
  takes the CO5300 through DISPOFF and SLPIN (`jr_display_panel_off_request`,
  applied inside `panel_flush` because that is the only place a panel command
  may be issued); then the wake sources.
- **Wake sources, each armed only if its line is quiet at that moment**, so a
  wrong polarity costs a wake source and never a boot loop: ext0 on GPIO21
  level high (IMU), ext1 on GPIO11 any-low (touch, with the RTC pull-up kept
  on through `ESP_PD_DOMAIN_RTC_PERIPH`), and a timer, always. Both GPIOs are
  RTC-capable (0–21 on the S3). The AXP2101 PWR key cannot wake the S3 from
  deep sleep on this board.
- **RTC memory carries the way back:** whether the mic was live
  (`s_rtc_was_listening`), how many sleeps, and which sources were armed. A
  lifted device boots straight to `LISTENING`; a timer wake resumes the ladder
  at DREAM so the glass stays dark and the chip sleeps again in ten minutes
  unless something happens; a device that was muted stays muted.
- **From a desk:** `GET /api/debug/sleep` → `{"wake":"lift|touch|timer|power",
  "sleeps":n,"armed":{"lift":..,"touch":..},"lift_fail":"none|arm|high"}`.
  `POST /api/debug/sleep?now=1&wake_s=45` forces a sleep with a 45-second
  timer. A wake before the timer means a line was already at its wake level;
  a timer wake means both were quiet.

## Findings

| Date | Finding | Evidence |
|---|---|---|
| 2026-09-01 | Forced sleep at 80 s uptime with a 45 s timer: HTTP gone in 10 s, back in 51 s, `wake: timer`, `armed: lift true, touch true`, IMU sampler live after the wake with fresh samples | PLAN.md N10.4 |
| 2026-09-01 | **A deep sleep during OTA probation rolls the image back.** Deep sleep is a reboot through the bootloader; an image still `PENDING_VERIFY` is treated as a failed boot. The first forced test woke on the previous firmware. Now `enter_deep_sleep()` refuses while the running image is in probation and the debug route answers `409` | `main/main.c` `image_in_probation()` |
| 2026-09-01 | **The QMI8658 on this board never sets `STATUSINT.CmdDone`** for any CTRL9 command, with either `CTRL8` handshake type, over 100 ms of polling. The command still lands (the wake line and the readback agree), so the handshake is logged and not fatal; the engine is cleared at boot with a soft reset (`0x60 = 0xB0`) instead of a command. Revision register reads `0x7C` | `components/jr_imu/src/jr_imu.c` `imu_ctrl9()` |
| 2026-09-01 | Die temperature 43 °C at rest on the cell with Wi-Fi up; the owner's "it is hot" was the enclosure over charging + AMOLED + a 240 MHz core that never scaled | STATUS `CHIP` row |
| 2026-09-01 | **Idle share per core, awake and muted, 240 MHz:** core 0 ≈ 43 % idle with the renderer alone ≈ 50 % of it; core 1 ≈ 96 % idle (≈ 72 % with a session open, the AFE on it). **At 160 MHz:** core 0 37 % idle, renderer 55 %, 16 fps, a spoken reply with one 12 ms hole. 80 MHz would starve the renderer; 160 is the rest gear. Enabling `CONFIG_PM_ENABLE` + run-time stats cost ≈ 5 KB of free internal RAM; the largest block stayed 32 KB | `/api/diag/tasks` (`run` counters, `total_runtime`), `/api/debug/gain?cpu=` |

## Open questions

- The lift wake has been proven armed and quiet, not proven by a hand: a
  device off USB, face-down ten minutes, then lifted. Expect `wake: lift`.
  If it reads `timer`, lower `SLEEP_WOM_MG`.
- Real current in each mode is unknown; the PMIC cannot report it. A
  percent-over-time reading across a night on the cell is the practical proxy
  (PLAN.md N10.3 acceptance).
- Dynamic scaling (min 80, max 240 with locks) is still untried; the gears are
  the measured, lock-free version of it. The renderer is the daytime load —
  a lower render cadence at rest would save more than any clock.

## Sources

- `main/main.c` — `enter_deep_sleep()`, the mood tick's sleep decision,
  `debug_sleep_handler`, boot wake handling in `app_main`.
- `components/jr_core/include/jr_core/mood.h` — the ladder and
  `JR_MOOD_SLEEP_MS`; `host/test_mood.c` pins the rule both ways.
- `components/jr_imu/src/jr_imu.c` — `jr_imu_arm_wake_on_motion()`.
- `components/jr_net/src/jr_net.c` — `jr_net_set_power_save()`,
  `jr_net_power_save_active()`.
- ESP-IDF 5.5.4 `esp_sleep.h` — `esp_sleep_enable_ext0_wakeup`,
  `esp_sleep_enable_ext1_wakeup_io` (`ESP_EXT1_WAKEUP_ANY_LOW` on the S3).

## See also

[imu-interrupt-routing.md](./imu-interrupt-routing.md) · [board-175c.md](./board-175c.md) · `docs/GLASS_DESIGN.md` §F4
