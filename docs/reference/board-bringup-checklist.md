# Board bring-up: calibrate EVERYTHING physical, day one

**Status:** doctrine, written 2026-08-27 after the 1.75C port. The pins ported
in an afternoon; the *calibrations* leaked out as user-facing failures over the
following hours because each was assumed to transfer from the previous board.
None did. On any new board revision, run this sweep BEFORE calling the port done.

| Check | How | 1.75C outcome (why this list exists) |
|---|---|---|
| IMU axes (all three, signed) | Hold each face up/down, read `/api/sensors` raw ax/ay/az | Z inverted → device believed face-down from boot: instant DREAM, phantom privacy mute, wake gated off |
| Speaker chain gain | Long reply, playback tap `clipped_samples` + peak | Old 4x make-up gain railed 902/48000 samples → crackle heard as "choppy" |
| Mic normal PGA | Idle + spoken rms via `/api/gemini/live` mic_rms | OK by luck |
| Speak-phase mic PGA | vadlog during Speaking: user talk-over rms vs echo residual | Old 9 dB left the owner at ~20 rms — both barge paths stone deaf |
| Echo clip ceiling | Raise speak PGA until mic-raw rails during playback | 24 dB rails (244 samples); 21 dB is the C's optimum |
| Barge gate ratio | vadlog: residual/playback ratio | Old 0.30 (tuned for 28% AEC leak) vs measured 0.5% leak → gate 6x above any human voice |
| Acoustic output integrity | Self-listen: device mic vs playback tap holes | Feeder starvation gaps (420 ms) invisible to electrical taps |
| Battery/PMIC, RTC, expander presence | Boot log probes | AXP present, PCF85063/TCA9554 gone (handled) |

Rule: **a board swap invalidates every number derived from physics** — gain,
orientation, timing, thermal. Documentation ports pins; only measurement ports
calibration. The runtime knobs (`/api/debug/gain`: mic/ref/vol/speakmic/pbgain)
exist so the sweep needs no reflash loops; bake results as `#if` board defaults.
