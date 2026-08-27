# PLAN — Voice-UX parity on the 1.75C ("native Gemini feel")

Working plan, 2026-08-27. Milestones 1 (board port) and 2 (wake word) shipped
(`56a799b`, `9dcfe99`). This plan closes the experience gap the owner called
out: fluid speech, native-style barge-in, and a device that is NEVER deaf.

| # | Task | Status | Acceptance criteria |
|---|------|--------|---------------------|
| 1 | Deaf-Idle re-arm: any session death not caused by a deliberate mute re-arms voice (with backoff) in always-ready | DONE (pending flash) | Force a session death (kill Wi-Fi 30 s or wait for StaleDeadline); device returns to Listening without touch within 60 s |
| 2 | Wake watch as recovery net: gate on "not deliberately muted" instead of rest-ladder-only | DONE (pending flash) | With session dead and voice off (non-deliberate), saying "Jarvis" wakes + re-arms; after tap-mute or flip, "Jarvis" does NOT wake |
| 3 | Bake `speakmic=21` as C default (runtime tune lost on every reboot) | DONE (pending flash) | Reboot → vadlog during Speaking shows user talk-over crossing the barge gate without re-tuning |
| 4 | Barge-in end-to-end | READY FOR OWNER TEST | Every stage now measured: speak-mic 21 dB (user ~100-400 rms vs residual ~20), gate 0.10 (≈420), server START_SENSITIVITY_HIGH. vadlog + `interrupted` + stop <500 ms confirm |
| 5 | Crackle at 2.5x gain | READY FOR OWNER TEST | Electrical taps clean (0 clipped); owner ear confirms |
| 5b | Speaker output holes (feeder starvation) | FIXED `11e66a8` | Feeder prio 5→19 + internal stack; acoustic re-measure shows no >170 ms unexplained gaps (was 420 ms) |
| 5c | Full-gain speak-mic experiment | CLOSED — NOT VIABLE | 24 dB rails the echo (244 samples) and residual jumps 20→229 rms; 21 dB is the C's physical optimum, recorded in commit `11e66a8` |
| 6 | Session lifetime: reconcile `goAway` / `sessionResumption` / activity-detection config against current Live API docs | RESEARCH LANDED — implement compression/resumption | Device holds a session (or resumes seamlessly) for 30+ min unattended; no deaf windows |
| 7 | Mystery reboot (suspect USB brownout at peak speaker+Wi-Fi current) | WATCH | Coredump partition blank = no panic; capture `rst:` banner on next natural reboot before chasing |
| 8 | Consolidated owner test | BLOCKED on 1-5 flash | One session: converse, interrupt mid-reply, let it sleep, wake by voice — all four feel native |

## Elevation wave 2 (2026-08-27 evening — owner directives)

| # | Task | Owner | Status |
|---|------|-------|--------|
| E1 | Seconds hand on the watch | backend | SHIPPED `05f4bc0`, verified on glass |
| E2 | Remote canvas (any image → glass, TTL) + send-canvas.py | backend | SHIPPED `05f4bc0`, first push captured |
| E3 | Tap redesign: tap-while-speaking = stop-talking, never privacy mute (trap killed) | backend | CODED, awaiting joint flash |
| E4 | Double-tap = attention; swipe-left = status glance; swipe-right = watch peek | backend | CODED, awaiting joint flash |
| E5 | Operator lease (/api/operator/lease, owner tap reclaims) + jarvisctl lease/release | backend | CODED, awaiting joint flash |
| E6 | jarvisctl operator CLI + CLAUDE.md doc | backend | SHIPPED `500b297`, live-tested |
| E7 | Whole front experience: watch↔face fade, canvas easing, gesture visuals, wake bloom | glass-ux teammate | IN PROGRESS (jr_display domain) |
| E8 | Joint build/flash/verify of E3-E5+E7, owner look | backend | BLOCKED on E7 handover |

## Elevation backlog (after reliability — reliability IS the elevation)

| Idea | Note |
|---|---|
| Server VAD sensitivity tuning (`automaticActivityDetection`) | Make Gemini itself hear talk-over — primary barge path, local gate as fallback |
| Session resumption + context compression | Always-on desk presence without session cliffs |
| AXP2101 PKEY power-button gesture | C-board unlock (button readable via I2C now) |
| Wake feedback moment ("YES?" bloom + chirp) | The wake already works; make it delightful |
| Boot-to-ready time budget | Measure + trim the 5 s cold boot |
