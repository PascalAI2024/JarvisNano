# jarvis_brain

Persistent on-device **self** for the JarvisRobot ESP32-S3 firmware. Gives
JARVIS a continuous local identity + memory on the SD card so he is the *same
JARVIS* each session, not a stranger. Loaded into the LLM system instruction at
session start; learnings appended back after.

Implements the "His brain (self)" layer of `docs/BRAIN_ARCHITECTURE.md`: the
thin durable body holds identity + personal memory locally (survives reboots
**and reflashes** — the SD is not wiped by flash), while the cloud LLM does the
reasoning, flavoured by what the device knows about itself and the user.

## Storage layout (`/sdcard/brain/`)

| File | Role | Lifecycle |
|------|------|-----------|
| `identity.md` | Persona / who-he-is. Seeded on first boot with the JARVIS character; **user-editable on the SD to tune him without reflashing**. | Created if absent; never overwritten. |
| `memory.log` | Append-only timestamped interaction/learning notes. | Rotated once to `memory.1.log` past 64 KB so it can't fill the card. |
| `facts.md` | Optional curated durable facts. | Read if present; never created. |

`jarvis_brain_load_context()` composes: `identity.md` → optional `facts.md`
head → recent tail of `memory.log`, capped at **2 KB** total so it fits a
system instruction without bloating the WSS setup frame.

## Design — fail-soft everywhere

This component must **never** hang or crash the boot/voice path. The contract:

- `jarvis_brain_init()` **always returns `ESP_OK`.** If the SD or `/sdcard/brain`
  is unavailable it stays "not ready" and logs a warning; boot continues.
- `jarvis_brain_load_context()` **always returns `ESP_OK`** and **always writes a
  NUL-terminated, usable instruction.** When the SD is absent or `identity.md`
  can't be read, it copies a built-in `DEFAULT_PERSONA` (the same text used to
  seed `identity.md`, so the spoken persona is identical either way).
- `jarvis_brain_remember()` is best-effort: `ESP_OK` on write, `ESP_FAIL` if it
  couldn't persist (callers may ignore it). Never aborts.

No PSRAM ring, no writer task, no spinlocks — writes are occasional and
synchronous (`fopen`/`fputs`/`fclose`). **All reads are bounded** (head reads
cap at the buffer size; the `memory.log` tail seeks to end and reads back at
most the scratch size, then skips the first partial line). Persistence uses
`fflush` only — deliberately **no `fsync`**, so the source stays free of
`<unistd.h>` and the `-Werror=implicit` trap; best-effort durability is the
contract. Memory-log lines are prefixed with `esp_timer_get_time()/1000`
(monotonic ms since boot) — no NTP / wall-clock dependency.

## Public API (`include/jarvis_brain.h`)

```c
esp_err_t jarvis_brain_init(const char *base_dir);            // "/sdcard"; NULL -> "/sdcard". Always ESP_OK.
esp_err_t jarvis_brain_load_context(char *out, size_t out_sz);// fills <=2KB system instruction; always ESP_OK + NUL-terminated.
esp_err_t jarvis_brain_remember(const char *note);            // best-effort append; ESP_OK / ESP_FAIL.
bool      jarvis_brain_ready(void);                           // SD brain dir present + usable?
```

## Tunables (top of `src/jarvis_brain.c`)

| Macro | Default | Meaning |
|-------|---------|---------|
| `JB_CONTEXT_MAX` | 2048 | Hard cap on the composed system instruction |
| `JB_IDENTITY_READ_MAX` | 1536 | Bytes of `identity.md` read (most of the budget) |
| `JB_MEMORY_CAP_BYTES` | 64 KB | Rotate `memory.log` past this |
| `JB_NOTE_MAX` | 512 | Max bytes of one remembered note |
| `JB_DIR_MAX` / `JB_PATH_MAX` | 96 / 128 | Path buffers (mirror jarvis_logger; format-truncation-safe) |

---

## What the orchestrator must wire (NOT done by this component)

> All firmware edits below land in the **git-tracked vendored sources** under
> `firmware/components/...`, NOT the gitignored `esp-claw/` clone — `bootstrap.sh`
> copies them in on every run, so editing the vendored copy is what survives a
> clean re-clone. The `main.c` + main `CMakeLists.txt` changes are the exception:
> those files live only in the upstream clone and are applied via `bootstrap.sh`
> patch functions (see step 4).

### 1. `main.c` — init after SD mount

In the SD-mount-success branch (the `sdcard_probe()` path, next to the existing
`jarvis_logger_init("/sdcard")` call), add:

```c
#include "jarvis_brain.h"
...
jarvis_logger_init("/sdcard");
jarvis_brain_init("/sdcard");   /* persistent identity + memory; fail-soft */
```

Calling it before mount is harmless (it just stays not-ready), but place it
after the mount so the seed write lands on the card.

### 2. `cap_gemini_live.c` `gl_send_setup()` — swap the static persona for SD context

Edit the **vendored** copy:
`firmware/components/cap_gemini_live/src/cap_gemini_live.c`.

Add the include near the top:

```c
#include "jarvis_brain.h"
```

The current `systemInstruction` block in `gl_send_setup()` (≈ lines 385–389) is
**verbatim**:

```c
    cJSON *si   = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *parts = cJSON_AddArrayToObject(si, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", GEMINI_PERSONA);
    cJSON_AddItemToArray(parts, part);
```

Replace it with (loads identity + memory from SD; `cJSON_AddStringToObject`
copies the string, so the stack buffer is safe to let go afterwards):

```c
    char persona[2048];
    jarvis_brain_load_context(persona, sizeof persona);  /* always succeeds; falls back to default persona */

    cJSON *si   = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *parts = cJSON_AddArrayToObject(si, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", persona);
    cJSON_AddItemToArray(parts, part);
```

Notes:
- The `GEMINI_PERSONA` `#define` (≈ lines 68–71) becomes **vestigial**. Either
  delete it or keep it commented as a reference — don't leave silent dead code.
  Its text now lives in `jarvis_brain.c`'s `DEFAULT_PERSONA`/`IDENTITY_SEED`.
- `persona[2048]` is a stack buffer in `gl_send_setup()`, which already builds a
  cJSON tree on the stack. 2 KB is fine for the current task stack; if anyone
  later enlarges `JB_CONTEXT_MAX`/this buffer, verify the calling task's stack
  size first.

### 3. `cap_gemini_live` CMakeLists — add the dependency

In `firmware/components/cap_gemini_live/CMakeLists.txt`, add `jarvis_brain` to
`REQUIRES`:

```cmake
    REQUIRES
        claw_cap
        claw_core
        json
        ...
        jarvis_brain     # on-device identity + memory for the system instruction
```

### 4. `bootstrap.sh` — vendor the component + patch main's REQUIRES

**(a)** Add `copy_jarvis_brain()` mirroring `copy_jarvis_logger()` (vendored
source → the generated tree's local project components dir, on the IDF search
path):

```bash
copy_jarvis_brain() {
    # Persistent on-device identity + memory on the SD card. Same idiom as
    # copy_jarvis_logger: canonical source is git-tracked under
    # firmware/components/jarvis_brain and copied into the generated tree's
    # local project components dir so it survives a clean re-clone. main.c calls
    # jarvis_brain_init("/sdcard") after SD mount; cap_gemini_live loads the
    # system instruction from it via jarvis_brain_load_context().
    local src="$ROOT/firmware/components/jarvis_brain"
    local dst="$ESP_CLAW_DIR/application/edge_agent/components/jarvis_brain"
    [ -d "$src" ] || die "missing vendored jarvis_brain source at $src"
    log "copying jarvis_brain component → upstream tree"
    mkdir -p "$dst/src" "$dst/include"
    cp -f "$src/CMakeLists.txt" "$dst/CMakeLists.txt"
    cp -f "$src"/include/*.h "$dst/include/"
    cp -f "$src"/src/*.c "$dst/src/"
}
```

Call it in `main()` next to `copy_jarvis_logger`:

```bash
    copy_cap_gemini_live
    copy_jarvis_logger
    copy_jarvis_brain
```

**(b)** The edge_agent **main** component must list `jarvis_brain` in its
`REQUIRES` so `main.c` can include the header (same concern handled for
cap_gemini_live by `apply_gemini_live_main_require_patch`, which edits
`$ESP_CLAW_DIR/application/edge_agent/main/CMakeLists.txt`). Either:
- extend that existing patch to also append `jarvis_brain` to the main
  `REQUIRES` list, **or**
- add a small `apply_jarvis_brain_main_require_patch()` alongside it.

`jarvis_logger` is reachable from main without an explicit REQUIRES because it
sits in the local project components dir; if `main.c` only *calls* the function
(no special linkage issue) the existing setup may already resolve it — but the
**header include** needs the component on main's REQUIRES path. Verify the
component resolves at configure time and add the REQUIRES entry if the build
reports an unknown `jarvis_brain.h`.

### Build / flash (orchestrator)

No build-toolchain changes. After wiring, the component is picked up by the IDF
component manager. Follow the session's standard recipe (idf:v5.5.1 +
`idf-component-manager==2.4.10`, `--flash-mode dio`). The brain is fail-soft, so
even if the SD is missing at flash time the voice path runs on the default
persona.
