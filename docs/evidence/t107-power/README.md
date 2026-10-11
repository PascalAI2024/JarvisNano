# t107 evidence — build passed, device wake pending

The [baseline and report](../t107-power-baseline.md) records the power inventory,
fixed-window baseline, uncertainty, and firmware/device gates. Evidence in
this directory excludes pairing credentials and LAN identity.

Final host suites passed 11,451 / 854 / 166 / 81 / 38. The first pinned build
refused an incomplete generated directory. Its failure is retained as history.
After the owner removed only that directory, the authorized `RECONFIGURE=1`
build passed on unchanged, tested firmware source. `image.json` records the
1,854,464-byte image, SHA-256, and 55.786% free in the smallest app partition.
The successful build tail and exit code are retained separately from the
earlier failed attempt. The pre-OTA readiness request times out; the owner must
wake the device with privacy still muted. Candidate upload and after power
windows/captures remain pending. No measured improvement is claimed.

The before firmware's immutable revision is unknown. Its PRISM watch differs
from this checkout. Measurements therefore compare the observed running image
with the whole candidate, without attributing savings to an individual edit.

Awake windows last 300 seconds, sampled every 15 seconds after 30 seconds
settling. All measurements require battery power, no charging, inactive lease
and privacy mute. The 300-second sleep timer has no HTTP polling while asleep;
endpoint readings include shutdown, cold boot and reconnect.

The summary uses five minute-block means, a linear voltage slope and an
indicative 95% residual interval on three degrees of freedom. A conservative
2 mV differential rounding budget is stated separately. Neither term bounds
sensor accuracy, correlated noise, state of charge, temperature or relaxation.
These short windows cannot establish runtime or current consumption.

PNG files are live framebuffer mirrors. They prove rendered content, not
physical panel emission. DREAM captures use a transient synthetic face-down
orientation separately from the battery windows. Listening-state power is
excluded. The audible spoken turn remains owner check pending: the parent
stops after muted device verification for the owner to unmute physically.

Wave 1: `w1_power`, `w1_display`, `w1_audio` inspected independent source areas.
Wave 2: `w2_power`, `w2_display`, `w2_audio` implemented disjoint file sets.
Wave 3: parent full gates and `w3_break_it` independent failure review.
Wave 4: `w2_audio` repairs retained microphone half-batches, automatic tap
cursor continuity and companion privacy revalidation found by review;
parent repeats full gates and owns device operations alone.
Seven unique subagents. The coordinator register is local temporary state;
the brief reserves the shared Jarvis board for the orchestrator.
