# PLAN — Voice-UX parity on the 1.75C ("native Gemini feel")

Working plan, 2026-08-27. Milestones 1 (board port) and 2 (wake word) shipped
(`56a799b`, `9dcfe99`). This plan closes the experience gap the owner called
out: fluid speech, native-style barge-in, and a device that is NEVER deaf.

| # | Task | Status | Acceptance criteria |
|---|------|--------|---------------------|
| 1 | Deaf-Idle re-arm: any session death not caused by a deliberate mute re-arms voice (with backoff) in always-ready | DONE (pending flash) | Force a session death (kill Wi-Fi 30 s or wait for StaleDeadline); device returns to Listening without touch within 60 s |
| 2 | Wake watch as recovery net: gate on "not deliberately muted" instead of rest-ladder-only | DONE (pending flash) | With session dead and voice off (non-deliberate), saying "Jarvis" wakes + re-arms; after tap-mute or flip, "Jarvis" does NOT wake |
| 3 | Bake `speakmic=21` as C default (runtime tune lost on every reboot) | DONE (pending flash) | Reboot → vadlog during Speaking shows user talk-over crossing the barge gate without re-tuning |
| 4 | Barge-in end-to-end | IN TEST | vadlog: ≥4 consecutive frames rms>gate during owner talk-over; serial shows local barge event OR server `interrupted`; audio stops <500 ms |
| 5 | Crackle at 2.5x gain | IN TEST | Owner ear check + playback tap `clipped_samples == 0` over a long reply |
| 6 | Session lifetime: reconcile `goAway` / `sessionResumption` / activity-detection config against current Live API docs | RESEARCH LANDED — implement compression/resumption | Device holds a session (or resumes seamlessly) for 30+ min unattended; no deaf windows |
| 7 | Mystery reboot (suspect USB brownout at peak speaker+Wi-Fi current) | WATCH | Coredump partition blank = no panic; capture `rst:` banner on next natural reboot before chasing |
| 8 | Consolidated owner test | BLOCKED on 1-5 flash | One session: converse, interrupt mid-reply, let it sleep, wake by voice — all four feel native |

## Elevation backlog (after reliability — reliability IS the elevation)

| Idea | Note |
|---|---|
| Server VAD sensitivity tuning (`automaticActivityDetection`) | Make Gemini itself hear talk-over — primary barge path, local gate as fallback |
| Session resumption + context compression | Always-on desk presence without session cliffs |
| AXP2101 PKEY power-button gesture | C-board unlock (button readable via I2C now) |
| Wake feedback moment ("YES?" bloom + chirp) | The wake already works; make it delightful |
| Boot-to-ready time budget | Measure + trim the 5 s cold boot |
