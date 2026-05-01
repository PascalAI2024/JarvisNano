# Architecture

How the firmware is organized once it boots, and how a single user
utterance flows through it.

## Subsystem map

```mermaid
flowchart TB
    subgraph HW[Hardware — XIAO ESP32-S3 Sense]
        MIC[PDM Mic<br/>MSM261D3526H1CPM]
        OV[OV2640 camera]
        FLASH[(8 MB QIO flash)]
        PSRAM[(8 MB octal PSRAM)]
        USB[USB-Serial-JTAG]
        ANT[Wi-Fi 2.4 GHz / BLE]
    end

    subgraph IDF[ESP-IDF v5.5]
        I2S[i2s_pdm driver]
        WIFI[wifi_manager + provisioning AP]
        FATFS[FATFS<br/>on storage partition]
        NETIF[lwip + mDNS]
    end

    subgraph BMGR[esp_board_manager codegen]
        PERIPH[periph cfg<br/>i2c · i2s · rmt]
        DEV[device cfg<br/>audio_adc · audio_dac · led_strip]
    end

    subgraph CLAW[ESP-Claw runtime]
        CORE[claw_core<br/>agent loop]
        ROUTER[claw_event_router]
        SCHED[cap_scheduler]
        MEM[claw_memory<br/>structured RAG]
        SKILLS[claw_skill<br/>51 skills loaded]
        LUARTI[Lua runtime<br/>chat-coding]
    end

    subgraph CAPS[Capability groups — 18 total]
        IM[IM gateways<br/>Telegram · QQ · Feishu · WeChat · Web]
        MCPC[MCP client]
        MCPS[MCP server :18791]
        FILES[files cap]
        TIME[time cap]
        WEB[web search cap]
        SYS[system cap]
    end

    subgraph EXT[External services]
        LLM[(LLM HTTPS API)]
        IMSRV[(IM platforms)]
        MCPNET[(other MCP peers)]
    end

    MIC --> I2S --> CORE
    USB --> CORE
    ANT --> WIFI --> NETIF
    FLASH --> FATFS --> CORE
    PSRAM --> CORE
    BMGR --> CLAW
    CORE <--> SKILLS
    CORE <--> ROUTER
    CORE <--> SCHED
    CORE <--> MEM
    SKILLS <--> LUARTI
    CORE <--> CAPS
    CAPS <--> EXT
    NETIF <-.-> EXT
```

## Single-utterance lifecycle

What happens when you speak to the robot:

```mermaid
sequenceDiagram
    participant U as User
    participant M as PDM Mic
    participant CR as claw_core
    participant LU as Lua skill
    participant LLM as LLM API
    participant TTS as TTS skill
    participant DAC as I²S DAC (Phase 2)
    participant IM as Telegram / Web IM

    U->>M: voice
    M->>CR: 16 kHz mono PCM
    CR->>CR: VAD + endpointing
    CR->>LLM: STT request (audio bytes)
    LLM-->>CR: transcript
    CR->>CR: claw_event_router rule match
    CR->>LU: dispatch matched skill
    LU->>LLM: chat completion (tools + memory)
    LLM-->>LU: tool calls + reply
    LU->>CR: execute tool calls
    CR->>IM: stream reply text
    IM-->>U: chat bubble
    CR->>TTS: synthesize reply
    TTS-->>DAC: PCM frames
    DAC-->>U: speaker audio (Phase 2)
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
    E --> F[BOARD_MANAGER<br/>I2C · I2S PDM-RX · I2S STD-TX]
    F --> G[FATFS mount + skills sync]
    G --> H[wifi_manager<br/>STA → AP fallback]
    H --> I[claw_event_router]
    I --> J[cap_scheduler]
    J --> K[CLI REPL on USB-Serial-JTAG]
    K --> L[idle / awaiting LLM creds]
```

## File layout (firmware side)

The `application/edge_agent/` ESP-IDF project is laid out as follows
(after `bootstrap.sh` runs):

```
edge_agent/
├── main/                        # app entry, Wi-Fi mgr, CLI
├── components/                  # capability impls (im, mcp, scheduler, memory…)
├── boards/                      # the YAML+C board adaptations
│   ├── espressif/               # upstream-provided
│   ├── m5stack/                 # upstream-provided
│   └── seeed/xiao_esp32s3_sense/  ← ours
├── managed_components/          # idf-component-manager pulls
│   └── espressif__esp_board_manager/  ← codegen patched here
└── partitions_8MB.csv           # 8 MB layout used on the XIAO
```
