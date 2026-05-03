# BLE bridge — Phase 2

Status: client skeleton only. UUIDs are canonical, but the firmware GATT
service and characteristic IO are not wired yet.

The `BleClient` here can scan and connect, but it does not yet read or write any
characteristic. The intent is a single GATT service that lets the phone act as a
direct microphone / speaker / control panel for the device, bypassing the LAN.

Runtime behavior is intentionally bounded:

- Scan runs for 10 seconds, trying a service UUID filter first on adapters that
  support offloaded filtering, then falling back to name-prefix matching.
- Connect/service discovery runs for 15 seconds before failing closed.
- GATT and scan status codes are surfaced in `BleClient.State.Failed` for the
  Cockpit BLE tile instead of silently returning to idle.

## Protocol

| Characteristic       | UUID                                     | Direction     | Payload                                                |
| -------------------- | ---------------------------------------- | ------------- | ------------------------------------------------------ |
| `service`            | `1ec185cd-4bc7-5797-a8b1-0f5b66c59757`   | service       | JarvisNano GATT service                                |
| `audio_in`           | `ca04b99f-5e74-5a35-8f4f-d1313f19b29b`   | device→phone  | PCM 16 kHz mono S16LE, 20 ms frames                    |
| `audio_out`          | `872228b7-ccd8-55dd-b12b-5d0352903617`   | phone→device  | PCM 16 kHz mono S16LE, 20 ms frames (TTS playback)     |
| `state`              | `dab5c3d4-915d-5f25-acc9-9d511df742bf`   | device→phone  | JSON `{mode, battery_pct, error?}`                     |
| `control`            | `2e14c0f2-4b07-5802-a8f9-369752d7cf2a`   | phone→device  | JSON commands (`start_listen`, `stop`, `restart`, …)   |

These values are derived in [`docs/PROTOCOL.md`](../../../../../../../../../docs/PROTOCOL.md)
and must match the firmware exactly.

## What "done" looks like for Phase 2

- [x] Scan + connect UI with permission prompts handled per Android 12+ runtime model.
- [x] Report whether the connected peripheral exposes the canonical JarvisNano GATT service.
- [x] Bounded scan/connect flow with service UUID filtering where supported.
- [ ] CCCD writes to enable notifies on `audio_in` and `state`.
- [ ] Audio in: pump the notify stream into an `AudioTrack`.
- [ ] Audio out: capture from `AudioRecord`, chunk to 20 ms frames, write to
      `audio_out` with response.
- [ ] Auto-reconnect on RSSI loss / disconnect.
- [ ] Battery-aware: pause audio capture below 20% to extend the conversation.

## Why BLE at all when we have Wi-Fi?

- Wi-Fi requires the device to keep a fully connected radio + DHCP lease, which
  burns ~3× the power of a connected BLE peripheral.
- A coffee shop / hotel network won't accept the device's captive-portal flow.
  BLE works anywhere the phone can hold a connection.
- Voice latency over BLE GATT is ~80 ms. Voice over Wi-Fi via WebSocket is
  reliably under 30 ms but unreliably 200+ ms when the access point is busy.

We keep both transports. Phase 1 = Wi-Fi (what we have now). Phase 2 = BLE for
mobility. Both speak the same chat / state model so the rest of the app doesn't
care which is in use.
