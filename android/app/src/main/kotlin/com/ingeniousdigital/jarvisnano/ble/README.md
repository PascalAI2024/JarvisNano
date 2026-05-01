# BLE bridge — Phase 2

Status: scaffolded, not wired.

The `BleClient` here can scan and connect, but it does not yet read or write any
characteristic. The intent is a single GATT service that lets the phone act as a
direct microphone / speaker / control panel for the device, bypassing the LAN.

## Planned protocol

| Characteristic       | UUID (placeholder)                       | Direction     | Payload                                                |
| -------------------- | ---------------------------------------- | ------------- | ------------------------------------------------------ |
| `audio_in`           | `0000A101-0000-1000-8000-00805F9B34FB`   | device→phone  | PCM 16 kHz mono S16LE, 20 ms frames                    |
| `audio_out`          | `0000A102-0000-1000-8000-00805F9B34FB`   | phone→device  | PCM 16 kHz mono S16LE, 20 ms frames (TTS playback)     |
| `state`              | `0000A103-0000-1000-8000-00805F9B34FB`   | device→phone  | JSON `{mode, battery_pct, error?}`                     |
| `control`            | `0000A104-0000-1000-8000-00805F9B34FB`   | phone→device  | JSON commands (`start_listen`, `stop`, `restart`, …)   |

The placeholder UUIDs above will be replaced with the real firmware UUIDs once
the GATT service lands in `firmware/`.

## What "done" looks like for Phase 2

- [ ] Scan + connect with permission prompts handled per Android 12+ runtime model.
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
