# Playback jitter buffer and earcons

**What it is** — The speaker runs behind the network. `components/jr_audio/src/jr_audio.c` keeps one playback ring, one feeder task (`jr_pb_feed`, priority 19, core 0) and an adaptive pre-roll: a reply's first word waits 600 ms so Gemini's 0.8–1.34 s mid-sentence stalls are absorbed (N6.16, 2026-09-01). The walk lives in the pure stepper in `components/jr_dsp` since wave N13 so a laptop can test it.

**How we use it here** — Every hole the feeder finds mid-reply steps the pre-roll up 300 ms (ceiling 1500); three consecutive clean replies step it down 200 ms (floor 600). `/api/device/health` → `playback` reports `underruns`, `max_gap_ms`, `replies`, `prerolls`; `POST /api/debug/gain?preroll=N` pins it (0 restores the walk).

---

## Findings & gotchas

**[2026-09-05] The mute earcon was booked as a reply and cost every real reply 300 ms.**
A glass long-press plays the falling mute sweep through the same ring and feeder as Gemini audio. The sweep was generated in 256-sample chunks (10.7 ms at 24 kHz) and enqueued with `diagnostic=true`, which only skipped the pre-roll hold — so the prio-19 feeder drained each chunk before the app task (prio 7) produced the next, the ring went dry inside the 80 ms DAC-tail window, and the feeder logged four `playback underrun: 9 ms hole mid-reply`. 2.5 s of silence then "ended the reply" and `pb_adapt_preroll()` stepped 600 → 900 ms. Health showed `replies:1, underruns:4` with no Gemini reply ever played. Evidence: `docs/evidence/20260905-earcon-preroll.log`. The 2026-09-01 record that the pre-roll "stepped 600→900 on its own after a hole" (PLAN N10.11) was almost certainly this, not a network stall.

Fix (wave N13): `pb_enqueue` counts outstanding earcon samples; while any are outstanding, or within the tail after the last one drained, the feeder neither opens a reply, nor books a hole, nor ends a reply, nor steps the pre-roll. Tones are rendered whole so they cannot starve their own ring. The walk is a pure `jr_dsp` stepper with `host/test_jitter.c`.

**[2026-09-05] The feeder had 524 bytes of stack left.**
`int16_t chunk[768]` (1536 B) lived on the 4096-byte internal stack of the one task that must never overflow, beside the `ESP_LOGW` vprintf on the hole path. `/api/diag/tasks` showed `jr_pb_feed stack_free 524`. The chunk is now a static internal buffer; the stack size is unchanged because internal RAM's largest block is the binding constraint.

**[2026-09-01] Gemini paces native audio near real time and stalls for seconds.**
A lead ≥ the stall is the only cure and costs the same in first-word latency; see `gemini-live-api-v5.md` §7 and PLAN N6.16 / N10.10.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `components/jr_audio/src/jr_audio.c` (`feeder_task`, `pb_enqueue`, `jr_audio_play_sweep`) | The ring, the feeder, the earcons. |
| `components/jr_dsp/src/dsp.c` (the jitter stepper) + `host/test_jitter.c` | The walk and its tests. |
| `main/http_routes.c` (`/api/device/health` `playback`, `/api/debug/gain`) | Counters and the knob. |
| `docs/evidence/20260905-earcon-preroll.log` | The device log that showed it. |

---

## Open questions

- N6.4's remaining gate (a 60 s reply with no unexplained gap > 120 ms) still needs a soak read against these counters now that earcons cannot pollute them.

---

## See also

- [gemini-live-api-v5.md](./gemini-live-api-v5.md) — server pacing measurements.
- [audio-es8311-es7210.md](./audio-es8311-es7210.md) — the codec path under the ring.
