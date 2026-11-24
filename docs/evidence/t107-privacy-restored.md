# t107 privacy recovery handoff

Recorded 2026-10-13 on `t107/codex`; source image remains `343e51c`.
This closes the mute-on-wake task. Voice acceptance on this image is parked
pending the owner; C1 acceptance remains incomplete.

The watcher finished **DONE_MUTED_AND_CAPTURED at 04:44:17.167581Z**.
First authenticated health at 04:44:15.729510Z already reported privacy true
and mic/ADC/DAC/PA off. Its only device write was software DISARM
(`POST /api/voice/control?armed=0`), acknowledged at 04:44:15.843564Z.
At 04:44:16.045146Z, health and cockpit agreed: muted, capture false,
voice armed false, disconnected voice transport, all four audio paths off.
Final health after the capture remained muted and `ota_1` valid.

The [opened screen capture](t107-privacy-restored.png) shows WATCH with the
gold privacy ring. It is a fresh 466×466 panel-submission mirror, not optical
glass readback. The [sanitized receipt](t107-privacy-restored.json) records
timestamps, capture hashes and hashes of the retained private evidence.
PID 64308 has exited; stderr is empty and the terminal event reports success.

The requested **23:33Z on October 12 through 04:44:17Z on October 13**
interval, 5h11m17s, remains a privacy-confirmation gap. The last successful
unmuted readback was 23:32:09.135603Z. The earlier software-mute attempt timed
out without an acknowledgement. Offline probes did not reveal privacy or
microphone state. This record cannot establish when mute was restored or
that the device listened continuously throughout the gap. No spoken-turn
acceptance was received.

The watcher ran from 01:11:31.028292Z to its first authenticated response at
04:44:15.729510Z: **3h32m44.701s**, 852 probes. Before that response it recorded
850 transport errors and one HTTP 404. Those observations establish API
unavailability, not an absence of hardware timer wakes.

The natural sleep path uses `SLEEP_TIMER_WAKE_S = 4 * 3600` (14400s), in
`main/app.h` and `main/main.c`. `main/power.c` arms the ESP timer with that
duration. The earlier 1800s request applies only to a forced debug sleep;
it does not replace the natural default. The timer boot branch retains the
muted default and resumes DREAM. Thus a roughly 3.5h API gap is **expected
within a four-hour sleep interval**, and the already-muted return is
compatible with timer boot. The watcher did not capture the wake cause,
sleep counter, reset reason or actual sleep-entry time. It cannot prove
that this return was a timer wake or pass the unattended default4h gate.
Follow-up read-only sleep/cockpit/health/device/log GETs at 04:59:49–05:00:09Z
all timed out, so that distinction remains unresolved. No timer defect is
established by this gap alone; no claim of a passing timer gate is made.

Stop here. No firmware, privacy-unmute, power or input write was made in
this recording turn. The remaining order is owner manual unmute, one voice
turn and re-mute with API confirmation; fresh matched unplugged HOME drain
windows of at least 45 minutes; unattended natural ladder/default4h timer;
then C2. CHARGE remains closed at 200mA with no PMIC register write.
