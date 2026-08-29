# JarvisNano roadmap

Last reconciled: **2026-08-28**. This is the product arc, not the task board.
Exact statuses and acceptance gates live in [`../PLAN.md`](../PLAN.md).

## Now — make the shipped experience release-grade

The 1.75C already behaves as an ambient desk assistant: direct Gemini Live
voice, server VAD, full-duplex audio, Watch, Desk/Tools/Settings, one controls
surface, privacy, operator takeover, diagnostics, and rollback-capable Wi-Fi OTA.

Current work removes the last gaps between a strong demonstration and a durable
product:

- Prove PWR, BOOT, edge controls, privacy, and recovery with one physical
  interaction circuit.
- Instrument playback starvation and complete a clean 30-minute conversation
  soak.
- Replace the temporary JarvisMCP result-count cap with a byte-budgeted,
  cursor-bearing projection.
- Raise controls cadence and prewarm first-use face assets.
- Collapse repository truth into current docs, one test command, and smaller
  ownership modules.
- Finish signed images, authenticated encrypted upload, attended at-rest secret
  protection, and exact third-party notices before public release.

## Next — efficient ambient presence

After reliability closes:

- Measure real current before enabling dynamic frequency scaling or light sleep.
- Prove QMI8658 INT2 electrically, then replace stillness polling without
  regressing flip, lift, or shake behavior.
- Use the unused FAT partition only for a documented durable-memory behavior;
  otherwise reclaim it through an explicit partition migration.
- Implement real Gemini session-resumption handles and verify context continuity
  across the provider’s session boundary.
- Add compositor-native cards and radial controls only when they fit the round
  glass, interaction grammar, frame budget, and internal-memory floor.

## Later — spatial and extensible interaction

- Explore two-microphone direction only after enclosure captures classify
  left/right speech above 90%.
- Add native motion signatures—iris sleep, Wi-Fi orbit, bounded show reel—through
  the existing compositor rather than a second renderer.
- Expand JarvisMCP tools conservatively: discoverable, typed, least-privilege,
  compact enough for the device, and fail-closed.
- Treat camera, SD, or board-family features as explicit hardware variants. The
  primary 1.75C has neither camera nor microSD.

## Product constraints

- Privacy is physical and local. Remote control cannot clear it.
- One compositor, one transport authority, one interaction map.
- No public-release security claim without positive and negative device proof.
- No feature closes on host tests alone when its contract is visible, audible,
  physical, or power-related.
