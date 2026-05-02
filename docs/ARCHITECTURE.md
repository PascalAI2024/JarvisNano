# Architecture

How the firmware is organized once it boots, and how a single user
utterance flows through it.

## Subsystem map

```mermaid
flowchart TB
    subgraph HW[Hardware XIAO ESP32-S3 Sense]
        MIC[PDM mic<br/>MSM261D3526H1CPM<br/>GPIO41/42]
        OV[OV2640 camera<br/>on-board DVP]
        SPK[PAM8002A amp<br/>+ 28 mm speaker<br/>via GPIO4 + RC LPF]
        LED[On-board user LED<br/>GPIO21 active-low]
        FLASH[8 MB QIO flash 80 MHz]
        PSRAM[8 MB octal PSRAM 80 MHz]
        USB[USB-Serial-JTAG USB-C]
        ANT[Wi-Fi 2.4 GHz + BLE 5.0]
    end

    subgraph IDF[ESP-IDF v5.5]
        I2S0[i2s_pdm driver<br/>full-duplex on I2S0]
        WIFI[wifi_manager<br/>STA + AP fallback]
        FATFS[FATFS on storage partition]
        NETIF[lwip + mDNS esp-claw.local]
        HTTP[esp_http_server<br/>port 80 + WS /ws/webim]
    end

    subgraph BMGR[esp_board_manager codegen]
        PERIPH[periph cfg<br/>i2c, i2s_audio_in/out, rmt]
        DEV[device cfg<br/>audio_adc, audio_dac, led_strip]
    end

    subgraph CLAW[ESP-Claw runtime]
        CORE[claw_core agent loop]
        ROUTER[claw_event_router<br/>JSON rules at /fatfs/router_rules]
        SCHED[cap_scheduler cron triggers]
        MEM[claw_memory structured RAG]
        SKILLS[claw_skill 51 skills loaded]
        LUARTI[Lua runtime<br/>chat-coding + status_led.lua]
    end

    subgraph CAPS[18 capability groups]
        IM[IM gateways<br/>Telegram, QQ, Feishu, WeChat, Web]
        MCPC[MCP client]
        MCPS[MCP server 18791]
        FILES[files cap]
        TIME[time cap]
        WEB[web search cap]
        SYS[system cap]
    end

    subgraph EXT[External]
        LLM[LLM HTTPS API<br/>MiniMax-M2.7 default]
        IMSRV[IM platforms]
        MCPNET[other MCP peers on LAN]
        DASH[browser dashboard<br/>+ Android + ZeroChat]
    end

    MIC --> I2S0
    I2S0 --> CORE
    CORE --> I2S0
    I2S0 --> SPK
    USB --> CORE
    ANT --> WIFI
    WIFI --> NETIF
    NETIF --> HTTP
    FLASH --> FATFS
    FATFS --> CORE
    PSRAM --> CORE
    BMGR --> CLAW
    CORE --> LED
    CORE <--> SKILLS
    CORE <--> ROUTER
    CORE <--> SCHED
    CORE <--> MEM
    SKILLS <--> LUARTI
    LUARTI --> LED
    CORE <--> CAPS
    CAPS <--> EXT
    HTTP <--> DASH
    NETIF <--> EXT
```

## I2S0 full-duplex layout

ESP32-S3 only supports PDM on **I2S0**, so we run mic + speaker as two
separate channels on the same controller:

```mermaid
flowchart LR
    subgraph S3[ESP32-S3 I2S0 controller]
        RX[RX channel PDM-RX mode]
        TX[TX channel PDM-TX one-line]
    end

    M[MEMS mic<br/>MSM261D3526H1CPM]
    R[270 ohm]
    C[100 nF to GND]
    A[PAM8002A IN+]

    M -->|CLK GPIO42| RX
    M -->|DATA GPIO41| RX
    TX -->|DOUT GPIO4| R
    R --> A
    R --- C
```

Verified by the boot log:

```
PERIPH_I2S: I2S[0] PDM-RX, clk: 42, din: 41
PERIPH_I2S: I2S[0] PDM-TX, clk: -1, dout: 4
PERIPH_I2S: I2S[0] initialize success: 0x3c1fcd90
PERIPH_I2S: I2S[0] initialize success: 0x3c1fcbbc
```

## Single-utterance lifecycle

What happens when you speak to the robot:

```mermaid
sequenceDiagram
    participant U as User
    participant M as PDM mic
    participant LED as Status LED
    participant CR as claw_core
    participant LU as Lua skill
    participant LLM as LLM API
    participant TTS as TTS skill
    participant DAC as PDM-TX speaker
    participant DASH as Dashboard / Android / ZeroChat

    U->>M: voice
    M->>CR: 16 kHz mono PCM on I2S0 RX
    CR->>LED: state LISTENING
    CR->>CR: VAD + endpointing
    CR->>LLM: STT request (audio bytes)
    LLM-->>CR: transcript
    CR->>LED: state THINKING
    CR->>CR: claw_event_router rule match
    CR->>LU: dispatch matched skill
    LU->>LLM: chat completion (tools + memory)
    LLM-->>LU: tool calls + reply
    LU->>CR: execute tool calls
    CR->>DASH: WebSocket frame
    DASH-->>U: shown in any connected client
    CR->>LED: state SPEAKING
    CR->>TTS: synthesize reply
    TTS-->>DAC: PCM frames
    DAC-->>U: speaker audio Phase 2
    CR->>LED: state IDLE
```

## Status LED state machine

The on-board GPIO21 user LED (active-LOW) is driven by an async Lua job
that runs forever once started. State controlled by the agent loop;
fallback patterns work even with no device events.

```mermaid
stateDiagram-v2
    [*] --> BOOT
    BOOT --> IDLE: 3 quick flashes
    IDLE --> LISTENING: mic VAD opens
    LISTENING --> THINKING: utterance complete → LLM dispatched
    THINKING --> SPEAKING: TTS playback starts
    SPEAKING --> IDLE: TTS done
    LISTENING --> IDLE: timeout / cancel
    THINKING --> ERROR: LLM failure
    ERROR --> IDLE: SOS triple-blink + pause
    IDLE --> IDLE: heartbeat wink every 2 s
```

## Why structured memory matters

ESP-Claw's `claw_memory` keeps facts on-device (FATFS). The system prompt
is small; relevant memories are retrieved per-turn. Privacy stays local;
only the prompt + relevant slice ever hits the LLM.

```mermaid
flowchart LR
    subgraph OnDevice[XIAO Sense FATFS]
        RAW[raw notes]
        IDX[FTS index]
        CTX[per-turn slice]
    end
    USR[user msg] --> SCAN[similarity scan] --> CTX
    RAW --> IDX --> SCAN
    CTX --> PROMPT[final prompt] --> LLM[(LLM)]
    LLM --> REPLY[reply]
    REPLY -.->|extract facts| RAW
```

## Boot order

```mermaid
flowchart TD
    A[ROM bootloader] --> B[2nd-stage bootloader]
    B --> C[octal_psram + flash detect]
    C --> D[heap init + PSRAM pool]
    D --> E[main_task]
    E --> F[BOARD_MANAGER<br/>I2C, I2S0 PDM-RX + PDM-TX, RMT]
    F --> G[FATFS mount + skills sync]
    G --> H[wifi_manager<br/>STA then AP fallback]
    H --> I[claw_event_router<br/>load /fatfs/router_rules/router_rules.json]
    I --> J[cap_scheduler]
    J --> K[http_server port 80<br/>+ WS /ws/webim<br/>+ MCP server 18791]
    K --> L[CLI REPL on USB-Serial-JTAG]
    L --> M[idle awaiting LLM creds<br/>OR claw_core ready]
```

## File layout (firmware side)

The `application/edge_agent/` ESP-IDF project is laid out as follows
(after `bootstrap.sh` runs):

```
edge_agent/
├── main/                              # app entry, Wi-Fi mgr, CLI
├── components/                        # capability impls (im, mcp, scheduler, memory…)
├── boards/                            # the YAML+C board adaptations
│   ├── espressif/                     #   upstream-provided
│   ├── m5stack/                       #   upstream-provided
│   └── seeed/xiao_esp32s3_sense/      #   ← OURS
├── managed_components/                # idf-component-manager pulls
│   └── espressif__esp_board_manager/  #   ← codegen patched here
├── fatfs_image/                       # FATFS contents baked into flash
│   ├── scripts/builtin/status_led.lua #   ← OURS
│   ├── router_rules/router_rules.json #   includes our boot_status_led rule
│   └── skills/                        #   capability docs for the LLM
└── partitions_8MB.csv                 # 8 MB layout used on the XIAO
```

## Public protocol surface

Everything a client (browser dashboard, Android app, ZeroChat, third-party)
talks to is documented in [`docs/PROTOCOL.md`](PROTOCOL.md). Versioned via
the `X-JarvisNano-Protocol: 1` header.

| Surface | Where | Used by |
| --- | --- | --- |
| HTTP REST | port 80 | dashboard · Android · ZeroChat |
| WebSocket `/ws/webim` | port 80 | dashboard · Android · ZeroChat |
| MCP JSON-RPC | port 18791 `/mcp_server` | other MCP peers |
| BLE GATT (Phase 2) | UUIDs in PROTOCOL.md | Android · ZeroChat |
| On-device LLM hand-off (Phase 3) | BLE audio + control | Android (Gemma 4 E4B local) |
