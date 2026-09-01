# JarvisNano roadmap

Last reconciled: **2026-09-01**. This is the product arc, not the task board.
Exact statuses and acceptance gates live in [`../PLAN.md`](../PLAN.md).

## Where it stands

The 1.75C behaves as an ambient desk assistant: direct Gemini Live voice with
server VAD and full-duplex audio behind an adaptive jitter buffer; tools by
voice through a device-enforced allowlist; a ring of live screens (Watch,
Weather, Status, Activity, and Desk while a companion is live) that can be
spoken aloud with a tap; physical privacy; a rest ladder that ends in deep
sleep on battery with lift, touch and a timer as the ways back; three session
watchdogs; and rollback-capable Wi-Fi OTA with probation.

## Now — prove what shipped, in hands

- Prove the lift wake by hand and record a night on the cell as
  percent-over-time; the sleep is proven from a desk, the lift is not.
- Watch the deaf-session watchdog in real use and tune its count.
- Cache the shell veil so ring screens render at the face's 19 fps.
- Give the update ring and the companion rim different hues.
- Close the release gates: diagnostics auth back on, signed images,
  authenticated encrypted upload, at-rest credential protection, exact
  third-party notices.

## Next — a device that knows more and costs less

- Measure current per power mode by proxy (the PMIC reports none) and decide
  DFS on the numbers, not on hope; light sleep stays out while the codec
  captures.
- Provision the typed JarvisMCP device route so the policy boundary moves from
  the device's allowlist to the server, and widen the tools conservatively:
  typed, least-privilege, compact enough for the device, fail-closed.
- Real Gemini session-resumption continuity across the provider's session
  boundary and the deaf-session reconnect.
- The ideas queued on the glass: a sunrise/sunset arc on WATCH, tilt parallax,
  a soft completion chime, an hourly rain warning, the QMI8658 tap engine as a
  knock gesture.
- Use the unused FAT partition only for a documented durable-memory behaviour;
  otherwise reclaim it through an explicit partition migration.

## Later — spatial and extensible interaction

- Two-microphone direction only after enclosure captures classify left/right
  speech above 90 %.
- Native motion signatures — iris sleep, Wi-Fi orbit, a bounded show reel —
  through the existing compositor, never a second renderer.
- Camera, SD, or board-family features as explicit hardware variants. The
  primary 1.75C has neither camera nor microSD.

## Product constraints

- Privacy is physical and local. Remote control cannot clear it.
- One compositor, one transport authority, one interaction map.
- Every screen shows a live reading; there is no settings page.
- No public-release security claim without positive and negative device proof.
- No feature closes on host tests alone when its contract is visible, audible,
  physical, or power-related.
