# JARVIS Brain Architecture — personal on-device self + company-brain access

> Status: May 2026 design sketch. Live v5 tools are `jr_tools` +
> [`PROTOCOL.md`](PROTOCOL.md). This file is not the current implementation.

## Core principle: thin durable body, evolving brain

The ESP32-S3 cannot run a capable model on-chip (local "AI" tops out at
wake-words / tiny classifiers). So intelligence is split, and **everything that
evolves lives off the constrained device**:

| Layer | Where | What | Evolves by |
|-------|-------|------|-----------|
| **Senses + body** | device | mic, speaker, AMOLED, touch, reactive face | OTA (rare) |
| **His brain (self)** | device SD card | identity/persona, personal memory, learned facts about the user, local Lua skills | writing to SD (no reflash) |
| **Reasoning** | cloud LLM (Gemini today) | the actual thinking, flavored by his local memory | swap model server-side |
| **Company brain** | JarvisMCP `jarvis.memory.*` (S3/Obsidian vault) | shared org knowledge, reached on demand | any agent/device contributes |

## His brain (on device, `/sdcard/brain/`)

This is what makes him *continuous* — the same JARVIS each session, not a
stranger. Owned locally, survives reboots/reflashes (SD is not wiped by flash).
- `identity.md` — who he is, persona, relationship with the user.
- `memory/` — append-and-curate personal memory (same pattern as the host
  `MEMORY.md` system): conversations, user preferences, learned facts.
- Lua skills — local reflexes via esp-claw's existing skill engine
  (`/sdcard/skills`, `lua_module_*`, `enabled_lua_modules` config). Droppable
  over HTTP, enabled via `POST /api/config`, no reflash.

**Mechanism:** at session start, load relevant local memory and inject it into
the LLM context ("here's who I am, here's what I know about you"). After the
session, write new learnings back to SD.

## Company brain (reached, not stored)

A library he walks into via `/act` → `jarvis.memory.search/read` on the shared
vault. Queried on demand; not carried around. Keeps "his stuff" personal and the
"company stuff" shared + current. Durable personal learnings can be **promoted**
up to the vault when they matter to others.

## Evolution layers (easiest → hardest)

1. **Dynamic tools (server-driven, no flash)** — at session start the device
   fetches its tool/function manifest from the server and builds Gemini
   `functionDeclarations` dynamically. Add a `jarvis.*` tool server-side → it
   appears on the device automatically. *Highest leverage, smallest change.*
2. **On-device Lua skills (no flash)** — fetch/enable local reflexes on SD.
3. **OTA self-update** — re-add an OTA-B partition (16MB has room; an earlier
   session reclaimed the unused `ota_1`) + `esp_https_ota` + auto-rollback.
   True firmware-level self-healing.
4. **Self-heal loop** — device reports health/anomalies up (already can, via
   `/act` + SD logging + `/api/logs`); server pushes a config fix (1), a skill
   (2), or schedules an OTA (3).

## Open questions for the user
- Autonomy: does he write to his own memory freely, or propose for approval?
- One device vs. a fleet sharing one company brain.
- Security: signing for OTA images + auth boundary for the tool manifest.

## What already exists here (so this is mostly wiring, not new infra)
SD + FATFS, the Lua skill engine, `POST /api/config` runtime reconfig, the
`/act` JarvisMCP bridge (40 services + the company vault), SD logging +
`/api/logs`. Missing pieces: dynamic tool manifest, the `/sdcard/brain/` memory
format + context injection, and (for #3) an OTA partition + image flow.
