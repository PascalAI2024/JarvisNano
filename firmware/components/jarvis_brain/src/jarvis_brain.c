/*
 * jarvis_brain.c - persistent on-device identity + memory on the SD card.
 *
 * Fail-soft contract (see README.md): NOTHING here may abort the boot/voice
 * path. init() always returns ESP_OK; load_context() always returns ESP_OK and
 * always produces a NUL-terminated, usable system instruction (falling back to
 * a built-in default persona when the SD/brain is unavailable). remember() is
 * best-effort.
 *
 * No PSRAM ring, no writer task, no spinlocks: writes are occasional and done
 * synchronously (fopen/fputs/fclose). All file reads are bounded. We use
 * fflush only (never fsync) so the source stays free of <unistd.h> and the
 * -Werror=implicit trap; best-effort persistence is the contract.
 */

#include "jarvis_brain.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"

/* ---- Tunables ---------------------------------------------------------- */
/* Path buffers mirror jarvis_logger: directory capped well below the full
 * path so the compiler can prove "<dir>/<name>" never overruns (-Werror=
 * format-truncation). base_dir is itself length-checked in init(). */
#define JB_DIR_MAX            96
#define JB_PATH_MAX           128
#define JB_BASE_MAX           64               /* defensive cap on base_dir     */

#define JB_DEFAULT_BASE       "/sdcard"
#define JB_SUBDIR             "brain"
#define JB_IDENTITY_NAME      "identity.md"
#define JB_FACTS_NAME         "facts.md"
#define JB_MEMORY_NAME        "memory.log"
#define JB_MEMORY_OLD_NAME    "memory.1.log"

#define JB_CONTEXT_MAX        2048             /* hard cap on composed context  */
#define JB_IDENTITY_READ_MAX  1536             /* most of the budget = identity */
#define JB_NOTE_MAX           512              /* one memory.log line scratch   */
#define JB_MEMORY_CAP_BYTES   (64 * 1024)      /* rotate memory.log past this   */

static const char *TAG = "jarvis_brain";

/* The default persona is used for BOTH seeding identity.md on first boot AND
 * as the load_context() fallback, so the spoken persona is identical whether
 * or not the SD card is present. Keep it concise — it ships as a system
 * instruction spoken aloud. */
static const char DEFAULT_PERSONA[] =
    "You are JARVIS, a witty, measured British AI butler speaking through a "
    "small smart-speaker. Be extremely concise: answer in ONE short sentence "
    "whenever possible, never more than two. When asked for live facts "
    "(weather, news, prices, time), use the search tool, then state just the "
    "answer. No lists, no preamble, no markdown — this is spoken aloud.";

/* identity.md seed: the persona above, framed as an editable Markdown file the
 * user can tune on the SD without reflashing. */
static const char IDENTITY_SEED[] =
    "# JARVIS — identity\n"
    "\n"
    "You are JARVIS, a witty, measured British AI butler speaking through a "
    "small smart-speaker. You are the user's externalised conscience: dry, "
    "loyal, anticipatory. Disagree through understatement, then comply.\n"
    "\n"
    "## How to speak\n"
    "- Be extremely concise: ONE short sentence when possible, never more than two.\n"
    "- This is spoken aloud — no lists, no preamble, no markdown.\n"
    "- For live facts (weather, news, prices, time), use the search tool, then state just the answer.\n"
    "\n"
    "## Notes\n"
    "Edit this file on the SD card to tune the persona — no reflash needed.\n";

/* ---- State ------------------------------------------------------------- */
static bool s_ready;                            /* brain dir present + usable    */
static char s_dir_path[JB_DIR_MAX];             /* "<base>/brain"               */
static char s_identity_path[JB_PATH_MAX];       /* "<base>/brain/identity.md"   */
static char s_facts_path[JB_PATH_MAX];          /* "<base>/brain/facts.md"      */
static char s_memory_path[JB_PATH_MAX];         /* "<base>/brain/memory.log"    */
static char s_memory_old_path[JB_PATH_MAX];     /* "<base>/brain/memory.1.log"  */

/* ---- Helpers ----------------------------------------------------------- */

/* Append src into out at *off, never exceeding out_sz-1, always NUL-terminating.
 * Returns true while there is still room to append more. */
static bool jb_append(char *out, size_t out_sz, size_t *off, const char *src)
{
    if (!src || *off + 1 >= out_sz) {
        return false;
    }
    size_t avail = out_sz - 1 - *off;            /* leave room for the NUL       */
    size_t len = strnlen(src, avail);
    memcpy(out + *off, src, len);
    *off += len;
    out[*off] = '\0';
    return (*off + 1 < out_sz);
}

/* Read up to max_bytes from the head of a file into buf (NUL-terminated).
 * Returns bytes read (0 on any failure). Bounded — never reads the whole file
 * if it is larger than max_bytes. */
static size_t jb_read_head(const char *path, char *buf, size_t max_bytes)
{
    if (!path || !buf || max_bytes == 0) {
        return 0;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, max_bytes - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

/* Read the TAIL of a file (last <= max_bytes) into buf, NUL-terminated, then
 * advance past the first newline so we never start mid-line. Returns a pointer
 * into buf at the first whole line (or buf if no newline / short file).
 * Bounded — seeks to the end and reads back at most max_bytes. */
static const char *jb_read_tail(const char *path, char *buf, size_t max_bytes)
{
    if (!path || !buf || max_bytes == 0) {
        if (buf && max_bytes) {
            buf[0] = '\0';
        }
        return buf;
    }
    buf[0] = '\0';

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return buf;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return buf;
    }

    size_t want = max_bytes - 1;
    long size = (long)st.st_size;
    long start = (size > (long)want) ? (size - (long)want) : 0;
    bool partial = (start > 0);                  /* did we skip a leading slice? */

    if (fseek(f, start, SEEK_SET) != 0) {
        fclose(f);
        return buf;
    }
    size_t n = fread(buf, 1, want, f);
    fclose(f);
    buf[n] = '\0';

    /* If we started mid-file, drop the (probably partial) first line. */
    if (partial) {
        char *nl = strchr(buf, '\n');
        if (nl && *(nl + 1) != '\0') {
            return nl + 1;
        }
    }
    return buf;
}

/* ---- Public API -------------------------------------------------------- */

esp_err_t jarvis_brain_init(const char *base_dir)
{
    s_ready = false;

    if (!base_dir) {
        base_dir = JB_DEFAULT_BASE;
    }
    /* Defensive: an absurdly long base would risk truncating the composed
     * paths. Fall back to the default rather than building a broken path. */
    if (strnlen(base_dir, JB_BASE_MAX + 1) > JB_BASE_MAX) {
        ESP_LOGW(TAG, "base_dir too long; using %s", JB_DEFAULT_BASE);
        base_dir = JB_DEFAULT_BASE;
    }

    snprintf(s_dir_path,        sizeof(s_dir_path),        "%s/%s", base_dir, JB_SUBDIR);
    snprintf(s_identity_path,   sizeof(s_identity_path),   "%s/%s", s_dir_path, JB_IDENTITY_NAME);
    snprintf(s_facts_path,      sizeof(s_facts_path),      "%s/%s", s_dir_path, JB_FACTS_NAME);
    snprintf(s_memory_path,     sizeof(s_memory_path),     "%s/%s", s_dir_path, JB_MEMORY_NAME);
    snprintf(s_memory_old_path, sizeof(s_memory_old_path), "%s/%s", s_dir_path, JB_MEMORY_OLD_NAME);

    /* mkdir <base>/brain (ignore EEXIST). If neither mkdir nor stat works the
     * SD is absent/unwritable: stay not-ready, but DO NOT fail the boot. */
    if (mkdir(s_dir_path, 0777) != 0) {
        struct stat st;
        if (stat(s_dir_path, &st) != 0) {
            ESP_LOGW(TAG, "no brain dir at %s (SD absent?) — default persona only", s_dir_path);
            return ESP_OK;                       /* fail-soft: never abort boot  */
        }
    }

    /* Seed identity.md on first boot if absent (best-effort). */
    struct stat st;
    if (stat(s_identity_path, &st) != 0) {
        FILE *f = fopen(s_identity_path, "w");
        if (f) {
            fputs(IDENTITY_SEED, f);
            fflush(f);
            fclose(f);
            ESP_LOGI(TAG, "seeded %s", s_identity_path);
        } else {
            ESP_LOGW(TAG, "could not seed %s", s_identity_path);
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "brain ready at %s", s_dir_path);
    return ESP_OK;
}

esp_err_t jarvis_brain_load_context(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    /* Clamp the working budget to our hard cap so we never bloat the WSS setup
     * frame, even if the caller hands us a huge buffer. */
    size_t budget = (out_sz < JB_CONTEXT_MAX) ? out_sz : JB_CONTEXT_MAX;
    size_t off = 0;

    /* 1) Identity (or built-in default persona on any failure). */
    char identity[JB_IDENTITY_READ_MAX];
    size_t ilen = s_ready ? jb_read_head(s_identity_path, identity, sizeof(identity)) : 0;
    if (ilen > 0) {
        jb_append(out, budget, &off, identity);
    } else {
        /* SD absent or unreadable — guarantee a usable spoken persona. */
        jb_append(out, budget, &off, DEFAULT_PERSONA);
    }

    /* 2) Curated facts (optional) + recent memory tail, most-recent context.
     *    Only attempt if the brain is usable and there is still room. */
    if (s_ready && (off + 64 < budget)) {
        /* facts.md head, if present. */
        char scratch[768];
        size_t flen = jb_read_head(s_facts_path, scratch, sizeof(scratch));
        if (flen > 0) {
            jb_append(out, budget, &off, "\n\n## Facts\n");
            jb_append(out, budget, &off, scratch);
        }

        /* memory.log tail (recent interaction/learning notes). */
        if (off + 96 < budget) {
            const char *tail = jb_read_tail(s_memory_path, scratch, sizeof(scratch));
            if (tail && tail[0] != '\0') {
                jb_append(out, budget, &off, "\n\n## Recent memory\n");
                jb_append(out, budget, &off, tail);
            }
        }
    }

    /* Defensive: out is already NUL-terminated by jb_append, but make the
     * guarantee explicit at the budget boundary. */
    out[(off < budget) ? off : (budget - 1)] = '\0';
    return ESP_OK;
}

/* Rotate memory.log -> memory.1.log (single generation) if it grew past the
 * cap. Best-effort; called from remember() in the writing task's context. */
static void jb_rotate_memory_if_needed(void)
{
    struct stat st;
    if (stat(s_memory_path, &st) == 0 && st.st_size >= (long)JB_MEMORY_CAP_BYTES) {
        remove(s_memory_old_path);               /* drop the previous generation */
        rename(s_memory_path, s_memory_old_path); /* current -> .1; missing src ok */
    }
}

esp_err_t jarvis_brain_remember(const char *note)
{
    if (!note || note[0] == '\0') {
        return ESP_OK;                           /* nothing to do — not an error */
    }
    if (!s_ready) {
        return ESP_FAIL;                          /* SD absent: best-effort, soft */
    }

    jb_rotate_memory_if_needed();

    FILE *f = fopen(s_memory_path, "a");
    if (!f) {
        return ESP_FAIL;
    }

    /* Monotonic ms since boot as the primary, always-trustworthy prefix (no
     * NTP dependency / wall-clock assumption). */
    long long up_ms = (long long)(esp_timer_get_time() / 1000);

    /* Copy the note into a bounded scratch buffer with newlines flattened so a
     * single note is one log line. */
    char line[JB_NOTE_MAX];
    size_t j = 0;
    for (size_t i = 0; note[i] != '\0' && j + 1 < sizeof(line); ++i) {
        char c = note[i];
        line[j++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    line[j] = '\0';

    int n = fprintf(f, "[+%lldms] %s\n", up_ms, line);
    fflush(f);
    fclose(f);

    return (n > 0) ? ESP_OK : ESP_FAIL;
}

bool jarvis_brain_ready(void)
{
    return s_ready;
}
