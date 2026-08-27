# BUGLOG — owner testing, 2026-08-27 evening

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

Live capture: 5-minute serial log running during owner testing
(`owner-test-serial.log` in session scratchpad); counters snapshotted before
and after. Device state is queryable any time: `scripts/jarvisctl.py status`.

