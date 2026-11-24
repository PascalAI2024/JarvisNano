# NATURAL-LADDER — bounded read audit

Parent: t107/Addendum C continued acceptance. Owner: a1_code. Register:
`.evidence/t107a/task-register.md`, NATURAL-LADDER. Source inspected on
`t107/codex` after C1 `0b9645d` and report `82b3102`; C2 `c4681ad` remains
outside the firmware image. Other agent's display repair is in progress.

## Result

No inactivity/read-route defect found. A continuously still, face-up, muted
device above the saver threshold reaches WHISPER after 5 s, DREAM after 15 min,
and deep-sleep eligibility after 25 min from its **last activity**, not boot.
Read-only cockpit, health, sensor, display, sleep and snapshot requests do not
poke the mood clock. The prior approximately 38 min uptime observation is
consistent with the actual last HOME time.

An untouched 45 min HOME/WHISPER-14 window is incompatible with the existing
ladder. No existing debug route freezes that mood or rest clock without state
changes. Parent accepted a maintained HOME comparison using identical existing
synthetic HOME double taps; no firmware repair or timeout change is needed.

## Evidence — ladder and actual elapsed time

- `components/jr_core/include/jr_core/mood.h:43-58`: DREAM 900000 ms, DREAM
  dwell 600000 ms, quiet 5000 ms; saver divides DREAM and dwell by four.
- `components/jr_core/src/mood.c:19-35`: DREAM takes precedence over quiet
  WHISPER. `:59-66` resets the inactivity clock on a poke; `:84-98` resets it
  on face-down/motion/busy input; `:103-107` requires the DREAM dwell.
- `main/main.c:2366-2394`: moving requires three fresh positive IMU polls;
  ordinary vibration can therefore restart the ladder. Sensor readings only
  report that state. `components/jr_imu/src/jr_imu.c:77-79,394-398` uses a
  five-sample motion window and >50 mg magnitude deviation. No threshold was
  changed or proposed.
- `main/main.c:2468`: cap65 gives AWAKE65, WHISPER14, DREAM5. Quiet privacy
  input skips AMBIENT. `:2516-2518` calls the natural path with the default
  timer, subject to use/uptime checks. `main/power.c:143-264` independently
  refuses probation, current OTA/operator ownership, stale/unknown USB
  absence, failed timer/panel acknowledgement, or audio shutdown failure.
- Parent-owned `.evidence/t107d/pre-ladder-logs-private.txt:199-226` records
  HOME at uptime1400939, WHISPER at1405959 (+5020 ms), DREAM at2301019
  (+900080 ms). At the prior health uptime2273379, only872440 ms
  (14 min32.44 s) had elapsed since HOME. Thus the sampled WHISPER is expected.
  The 80 ms DREAM excess is ordinary poll resolution, not an extended ladder.

## Evidence — read routes

Read actual handlers in `main/http_routes.c`: health `:471`, sensors `:1190`,
touch `:1244`, cockpit `:2700`, display diagnostic `:2977`, sleep `:3163`,
operator status `:4289`, snapshot metadata `:4344` and PPM `:4389`.

Health/display/sleep/operator status copy state. Sensors starts its samplers
idempotently, then copies cached readings; boot already starts both samplers
in `main/main.c:4327,4331`. `jr_power_start` and `jr_imu_start` return on an
already-started sampler. Snapshot handlers copy the rendered mirror and do
not create an input event or force a new render. Cockpit expires stale agent
presence and expired non-local display surfaces under their existing locks,
but does not claim/renew a lease, clear privacy or change `s_mood`.

The only HTTP mood poke is operator release
`main/http_routes.c:2034`, a state-changing path conditioned on privacy not
being held. It is not part of the measurement read protocol. The read requests
still consume CPU/network time; identical observer cadence belongs in the
comparison, and should not be mistaken for zero-load measurement.

## Accepted maintained-HOME protocol

Both images: privacy held, no capture/audio rails, face-up, no USB/charging,
no OTA/lease after probation, battery >20%, cap65/volume95, HOME setup and
300 s settle, then2700 s window with91 samples every30 s. Parent's
`.evidence/t107d/device_run.py:59-64` now performs HOME maintenance at sample
indices18/38/58/78: measured540/1140/1740/2340 s.

Each maintenance action closes the shade and uses
`POST /api/debug/input?kind=double&x=233&y=100`. It is the existing route,
not an unmute or lease. Preserve all event receipts, awake excursions, frame
increments and energy throughout the full window. Label separate quiet
interval frame-rate summaries explicitly. This is **maintained HOME**, not
unattended natural idle or continuous brightness14.

The rejected initial schedule at600 s after300 s settling reached exactly
the15 min DREAM boundary; its pre-maintenance WHISPER assertion could fail.
Parent preserved the abandoned pre-window attempt and relaunched r2 with the
corrected schedule. No measured samples were discarded.

Privacy proof chain (also checked in B `03864cc` with `git show`):

- `main/http_routes.c:3212-3284`: double injects two TAP events120 ms apart.
- `components/jr_hal/src/input_touch.c:548-561`: injection forcibly tags both
  events SYNTHETIC. Caller flags cannot remove that provenance.
- `main/main.c:3100-3101` derives physical from that flag; `:3152` pokes mood
  for every tap. `:3367-3390` double-tap HOME changes nav/rest/caption/mood,
  not privacy. `:3600-3604` rejects synthetic privacy holds; `:3675-3679`
  rejects synthetic shade privacy taps. `:2770-2779` also refuses RESUME
  while explicit privacy is held, covering a stale mood-owned pause flag.

Preconditions remain an idle HOME bench with no choice/guest surface/active
conversation. Those presentations can consume taps and change what HOME means;
the preserved receipts should fail the stage rather than silently alter it.

## Owner checklist tap targets

Read `.evidence/t107/OWNER-CHECKLIST.md`. Steps1-5 preserve privacy if contacts
are short taps, not holds. Steps6-7 intentionally hand-toggle privacy and
remain the owner's later voice stage. PWR/BOOT are explicitly prohibited;
PWR short would clear privacy (`main/main.c:2576-2582`).

- Status DETAIL dismissal: **short upper-centre tap, about(233,100)**. The
  sheet is y150..358 (`jr_display.c:3401-3402`), so this target maps to
  DISMISS (`:6520-6524`), then `shell_dismiss` (`main.c:1737-1755`) changes
  only nav/shade/caption. A centre tap inside the DETAIL sheet can request a
  spoken screen description; do not substitute that for dismissal.
- Glance exit: one **short centre tap**, release well before850 ms. Classified
  input clears Glance (`main.c:3134`); muted idle attention only shows a mute
  caption (`:3723-3729`) and cannot arm capture. The850 ms hold threshold is
  `components/jr_hal/src/input_touch.c:45`; crossing it deliberately toggles
  privacy on physical inner-disc holds. Double-tap HOME also preserves mute.
- Physical lift/touch wake restores listening only when RTC says it slept
  listening (`main.c:4579-4583`). Current muted sleep stores that bit false
  (`main/power.c:263`). Privacy stays held in this run.

## Focused offline verification

Private `NATURAL-LADDER-check.py` compiles the **actual** `mood.c` with
`gcc -std=c11 -Wall -Wextra -Werror -O2` in a network-disabled host-test
container with a read-only workspace mount. Log:
`.evidence/t107/NATURAL-LADDER-check.log`. Opened and verified exit0/PASS.

It checks exact quiet boundaries, saver DREAM225000/sleep375000 ms, the
rejected300+600 s boundary, the complete corrected2700 s protocol at20 ms
resolution, and timer-wake DREAM restoration. The idealized two-tap protocol
has20480 ms AWAKE65 and2679520 ms WHISPER14; this is **simulation of existing
source**, not observed board durations or an energy saving. Actual scheduling,
captions/network work and render bursts are in parent device receipts.

B and current mood source/header have identical Git blobs:
`e160eae2638c2868c7983c9185330586c404c85e` and
`e663832d954b4c0972d447a77078bb77be9527f3`, verified with
`git rev-parse 03864cc:<path>` and `git hash-object <path>`.
No source repair occurred; scratch mutations are unnecessary for this read
audit and do not turn offline timing into a device acceptance claim.

## Separate natural acceptance and remaining risk

After the maintained pair, start the unattended run from the last known
activity timestamp. Use read-only probes only: no HOME/debug input/forced
sleep/lease/levels/privacy writes. Keep it quiet, face-up, unplugged, above
saver threshold. From that last activity, expect DREAM at15 min and natural
sleep eligibility at25 min. If continuing directly after the window, its
last maintenance was roughly6 min earlier, so do not restart the expected
clock from the final read or from boot. Motion/busy activity is a genuine
restart and must remain visible in evidence.

`main/app.h:224` defines14400 s; natural entry passes it and
`main/power.c:165-173,257-264` arms/reports that interval before real sleep.
Require subsequent RTC counter increase, wake reason timer, valid authenticated
health/privacy/capture/USB readbacks, with untouched interval. API loss alone
does not prove sleep; Wi-Fi loss is also possible. API-return time includes
boot/network time and is an upper bound, not precise hardware timer latency.
Timer wake resumes DREAM (`main/main.c:4572-4577`) and can naturally sleep
again ten minutes later. Do not issue HOME just to keep a timer receipt alive.

Device natural-ladder/default4h proof is **pending parent execution**. Owner
physical/optical/voice acceptance remains pending. Acquisition/cache and a
physical USB insertion after final cached absence are the existing disclosed
sleep-entry limits; this audit did not remove them or infer measurements.

## Changed artifacts / decision

Only this report, private `NATURAL-LADDER-check.py` and its log were written
by this task. No production/test-suite source edit, device request, MCP call,
full gate or commit. Parent has accepted the corrected maintained protocol and
upper-centre status dismissal. **Decision needed: none for firmware.** Parent
continues the paired bench and separate unattended natural/default4h proof.
