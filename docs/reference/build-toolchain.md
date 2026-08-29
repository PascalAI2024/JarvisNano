# Build toolchain

Current, release-target reference for the 32 MB Waveshare 1.75C. The executable
recipe is [`../BUILD.md`](../BUILD.md); do not substitute the historical
ESP-Claw/bootstrap path.

## Pinned contract

| Tool/component | Version | Authority |
|---|---:|---|
| ESP-IDF Docker image | `espressif/idf:v5.5.4` | `scripts/build-v5.sh` |
| `idf-component-manager` | `2.4.10` | `scripts/build-v5.sh` |
| `esp-bmgr-assist` | `0.5.0` | `scripts/build-v5.sh` |
| Board manager | `0.5.15` | `main/idf_component.yml` |
| CO5300 driver | `2.1.0` | `main/idf_component.yml` |
| CST9217 driver | `2.0.0` | `main/idf_component.yml` |
| ESP-SR | `2.4.6` | `main/idf_component.yml` |
| codec-dev | `1.5.11` | `main/idf_component.yml` |
| WebSocket client | `1.7.0` | `main/idf_component.yml` |
| emote-gfx | `3.0.2` | `main/idf_component.yml` |

[`../../tools/idf-pins.txt`](../../tools/idf-pins.txt) is the compact review
copy. `dependencies.lock` is generated and ignored; the final build plus
`generate-third-party-notices.py` records the exact resolved graph.

## Canonical build

```bash
./scripts/build-v5.sh
./scripts/smoke-build.sh
```

The build performs this order inside the pinned container:

1. install the two pinned Python build tools;
2. set the ESP32-S3 target when configuration is absent or explicitly reset;
3. generate board-manager code for `esp32s3_touch_amoled_1_75c`;
4. reconfigure and synchronize generated `CONFIG_ESP_BOARD_*` symbols;
5. apply/check the small pinned managed-component fixes;
6. build the image and verify sdkconfig/managed-source drift.

The smoke gate then proves DIO, 80 MHz, 32 MB flash geometry; exact offsets;
non-empty bootloader, partition table, OTA metadata, application, emote, and
WakeNet artifacts; and the 4 MB application-slot ceiling.

## Hard constraints

- The release build refuses every board except the 1.75C. A mismatched image can
  black-screen or overrun the original 16 MB board.
- CO5300 QSPI ownership requires **DIO** flash mode. `flash-v5.sh` refuses QIO.
- `sdkconfig.defaults` is the project baseline. The selected board file declares
  only variant capability; `sync-v5-sdkconfig.py` bridges generated symbols.
- `components/gen_bmgr_codes/`, `managed_components/`, `sdkconfig`,
  `dependencies.lock`, and `build/` are generated. Never patch them as canonical
  source.
- `main/main.c`, `components/jr_*`, the 1.75C board definition, and
  `partitions_32MB.csv` are canonical.

## Host suites

```bash
cmake -S host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure

cmake -S components/jr_tools/host -B build-tools-host
cmake --build build-tools-host
ctest --test-dir build-tools-host --output-on-failure
```

These suites intentionally have no ESP-IDF include path. Core/transport/display
code that reaches into a driver or network header fails at compile time.

## Historical path

`scripts/bootstrap.sh`, `firmware/`, `patches/`, and the ignored `esp-claw/`
checkout preserve the pre-1.75C experiment. They are not release inputs. Its
history is indexed in [`../ARCHIVE/`](../ARCHIVE/README.md).
