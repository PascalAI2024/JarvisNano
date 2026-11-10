# Interface cues and the speaker mute

**What it is** — Four generated sounds (tick, confirm, dismiss, limit) played on a physical touch, and the rule that governs every short tone the firmware plays (`jr_audio_play_sweep`, `jr_audio_play_chime`, `jr_audio_play_cue`).

**How we use it here** — `components/jr_core/src/cue.c` (pure) decides whether a cue may play and renders the samples; `jr_audio_play_cue()` queues them on the same earcon lane the chime uses; `ui_cue()` in `main/main.c` builds the context and calls both.

---

## Findings & gotchas

**[2026-11-10] `s_muted` in jr_audio is the DAC's mute, not the owner's**
`s_muted` (`jr_audio.c`) is true at boot, after a session ends (`JR_CMD_MUTE_DAC_NOW`), after a barge-in and in sleep. The owner's mute is `s_voice_privacy_paused || s_flip_muted` in `main.c`; muting ends the session, which sets `s_muted`. A tone has to open the DAC to be heard, and `earcon_commit()` did it with `jr_audio_dac_unmute()`, which stores `s_muted = false` for good. After any chime or sweep the device reported itself unmuted (`jr_audio_dac_muted()` false, the health verdict stopped showing the mute) until the next session end.

**[2026-11-10] The fix is two rules, pinned in `jr_core/dac_mute.h`**
A tone is refused while the owner mute is set (`jr_audio_set_owner_mute_probe`, registered in `main.c`). A tone that plays opens a muted DAC only for its own length: `s_cue_restore` is set, and the feeder mutes the DAC again once the ring is empty and no tail is sounding. A real unmute or mute (`jr_audio_dac_unmute`, `sink_mute_now`) clears the flag. `jr_audio.c` cannot compile on the host, so `host/test_cue.c` pins the same transitions on a pure model. The mute and unmute acknowledgements on a long press go through `jr_audio_play_sweep_owner_ack()`, the one explicit exemption from the refusal; the diagnostic chirp (attention beat, self-test) is exempt too. The completion chime and the ask-miss sweep are refused under the owner mute.

**[2026-11-10] Synthetic input is marked at the HAL and carried by one bool**
`/api/debug/input` goes through `input_touch.c:557`, which ORs `JR_INPUT_FLAG_SYNTHETIC` into the event; the input loop in `main.c` already derives `physical` from it. Every `ui_cue()` call passes `physical`, and `jr_cue_allow()` refuses any other source. `main/tests/test_ui_cue.py` runs the production `ui_cue()` against the real gate and fails if the source stops being passed.

**[2026-11-10] Levels**
Peak amplitudes are 2 to 4 percent of full scale at volume 100 (-36 to -28 dBFS), against 8 percent (-22 dBFS) for the chime. The codec's own volume applies on top. Rendered files and measurements: `docs/evidence/audio-cues-2026-11-10/`.

**[2026-11-10] A cue is silent in any phase where the microphone is open**
`jr_cue_phase_blocks()` refuses LISTENING, THINKING, SPEAKING and ASKING. With always-ready listening that is most of an unmuted device's day, so cues mostly sound while idle, reconnecting or in backoff. This is the conservative reading of "never into an open microphone"; loosen it in one place if the AEC makes it safe.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `components/jr_audio/src/jr_audio.c` | `earcon_claim`, `earcon_commit`, `cue_settle_mute`. |
| `components/jr_core/src/cue.c` | gate, level, generator. |
| `host/render_cues.c` | renders the WAVs from the same generator. |

---

## Open questions

- Nobody has heard these on the speaker. Level, pitch and the dull low note on a small driver need an ear.
- `ui_sounds` is stored (NVS `app`/`ui_sounds`) and settable with `ui_sounds_set()`, but no route or screen sets it yet and the status JSON does not report it.

---

## See also

- [audio-es8311-es7210.md](./audio-es8311-es7210.md) — the codec and amplifier behind the DAC mute.
- [playback-jitter.md](./playback-jitter.md) — why earcons are tracked apart from replies.
