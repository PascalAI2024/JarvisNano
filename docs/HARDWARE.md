# Hardware

Pin map, schematic, BOM, and wiring diagrams for each phase.

## XIAO ESP32-S3 Sense at a glance

| Spec       | Value                                |
| ---------- | ------------------------------------ |
| MCU        | ESP32-S3R8 (dual-core 240 MHz)       |
| Flash      | 8 MB QIO @ 80 MHz                    |
| PSRAM      | 8 MB octal @ 80 MHz                  |
| Wi-Fi      | 2.4 GHz, BLE 5.0                     |
| USB        | Native USB-C (USB-Serial-JTAG)       |
| Mic        | MSM261D3526H1CPM PDM (on-board)      |
| Camera     | OV2640 2 MP (on-board, ribbon)       |
| Footprint  | 21 × 17.5 mm                         |
| Power      | 5 V via USB-C, 3.3 V LDO regulated   |

## Pin assignments (Phase 1 firmware)

```mermaid
flowchart LR
    subgraph BOARD[XIAO ESP32-S3 Sense]
        direction TB
        D0[D0 / GPIO1<br/>RMT WS2812 placeholder]
        D1[D1 / GPIO2<br/>I²S BCLK]
        D2[D2 / GPIO3<br/>I²S WS]
        D3[D3 / GPIO4<br/>I²S DOUT]
        D4[D4 / GPIO5<br/>I²C SDA]
        D5[D5 / GPIO6<br/>I²C SCL]
        D6[D6 / GPIO43 — UART TX]
        D7[D7 / GPIO44 — UART RX]
        D8[D8 / GPIO7 — free]
        D9[D9 / GPIO8 — free]
        D10[D10 / GPIO9 — free]
        ONBOARD41[GPIO41<br/>PDM mic DATA]
        ONBOARD42[GPIO42<br/>PDM mic CLK]
        LED21[GPIO21<br/>user LED]
    end

    subgraph PDM[On-board MEMS mic]
        MIC[MSM261D3526H1CPM]
    end

    subgraph AMP[Phase 2 — MAX98357A]
        BCLK[BCLK]
        LRC[LRC / WS]
        DIN[DIN]
        SPKP[SPK+]
        SPKN[SPK-]
    end

    subgraph SCREEN[Phase 3 — touchscreen]
        TSDA[I²C SDA]
        TSCL[I²C SCL]
    end

    MIC -- PDM CLK --> ONBOARD42
    MIC -- PDM DATA --> ONBOARD41
    D1 --> BCLK
    D2 --> LRC
    D3 --> DIN
    D4 --> TSDA
    D5 --> TSCL
```

## Wiring — Phase 1 (bare board)

Nothing to wire. Plug USB-C into your Mac, hit `./scripts/flash.sh`,
done. The on-board PDM mic and Wi-Fi are already on the chip.

## Wiring — Phase 2 (add a speaker)

Adafruit / generic **MAX98357A I²S 3 W amp** + 4 Ω 1–2 W speaker.

```mermaid
flowchart LR
    subgraph X[XIAO ESP32-S3 Sense]
        V5[5V]
        GND[GND]
        D1[D1 / GPIO2 — BCLK]
        D2[D2 / GPIO3 — WS / LRC]
        D3[D3 / GPIO4 — DIN]
    end

    subgraph A[MAX98357A]
        VIN[VIN]
        AGND[GND]
        BCLK[BCLK]
        LRC[LRC]
        DIN[DIN]
        SPK1[SPK+]
        SPK2[SPK-]
        GAIN[GAIN — leave floating = 9 dB]
        SD[SD — leave floating = on]
    end

    SPKR[(4 Ω 1 W speaker)]

    V5 --> VIN
    GND --> AGND
    D1 --> BCLK
    D2 --> LRC
    D3 --> DIN
    SPK1 --> SPKR
    SPKR --> SPK2
```

**Notes:**
- Power the amp from `5V` (the USB-C rail), not `3V3`. The 3.3 V regulator on the XIAO can't deliver 1 W audio peaks.
- Common ground is critical — tie `AGND` to the XIAO `GND`.
- `SD` (shutdown) and `GAIN` can stay floating for default behavior. Pull `SD` low to mute.
- After wiring, flip the `audio_dac` block in [`board_devices.yaml`](../boards/seeed/xiao_esp32s3_sense/board_devices.yaml) from `chip: none` to `chip: dummy` and rebuild.

## Wiring — Phase 3 (touchscreen)

A 2.8" or 3.2" SPI ILI9341 + XPT2046 resistive touch is the cheapest
path. The shared SPI bus on the XIAO has only one set of free SPI pins,
so the LCD and touch share MOSI/MISO/SCK with separate CS lines.

Pin sketch (subject to change once we crib LVGL bringup from the M5 CoreS3 board config):

| LCD pin   | XIAO pin  | Notes                          |
| --------- | --------- | ------------------------------ |
| VCC       | 3V3       |                                |
| GND       | GND       |                                |
| CS        | D8/GPIO7  | LCD chip-select                |
| RST       | D9/GPIO8  | reset                          |
| DC        | D10/GPIO9 | data/command                   |
| MOSI      | D6/GPIO43 | SPI MOSI (also UART TX)        |
| SCK       | D7/GPIO44 | SPI SCK (also UART RX)         |
| MISO      | not used  | LCD is write-only              |
| LED       | 3V3       | backlight always on for now    |
| T_CS      | TBD       | XPT2046 touch CS               |
| T_IRQ     | TBD       | optional touch IRQ             |

This phase will need a new `spi_display` peripheral entry in
`board_peripherals.yaml` and a `display_lcd` device entry — see the
`boards/m5stack/m5stack_cores3/` config for the LVGL pattern.

## BOM

| Phase | Item                         | Where           | Approx cost |
| ----- | ---------------------------- | --------------- | ----------- |
| 1     | Seeed XIAO ESP32-S3 Sense    | Seeed / Mouser  | $14         |
| 1     | USB-C cable (data + power)   | anywhere        | $5          |
| 2     | MAX98357A I²S amp            | Adafruit / AliEx | $5          |
| 2     | 4 Ω 1 W speaker (~28 mm)     | AliExpress      | $2          |
| 2     | Hookup wire / breadboard     | bin             | -           |
| 3     | 2.8" SPI ILI9341 + XPT2046   | AliExpress      | $8          |
| 3     | (optional) 3D-printed shell  | print           | filament    |

Total Phase 1: **~$19**. Total Phase 2: **+$7**. Total Phase 3: **+$8**.

## Power budget

Order-of-magnitude figures, USB-C 5 V input:

| State                     | Current  | Notes                          |
| ------------------------- | -------- | ------------------------------ |
| Wi-Fi idle                | ~80 mA   | both cores idling, mic off     |
| Listening (PDM RX)        | ~110 mA  | mic + DMA active               |
| Wi-Fi TX burst            | ~250 mA  | spike during chat upload       |
| Speaker @ 1 W (Phase 2)   | +200 mA  | through MAX98357A              |
| Touchscreen backlight     | +60 mA   | typical 2.8" panel             |

A standard 5 V / 1 A USB charger covers all phases comfortably.
