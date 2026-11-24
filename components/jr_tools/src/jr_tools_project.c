/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure C on-device JSON budget projector. Zero ESP-IDF headers so it compiles
 * and tests identically on host and on ESP32-S3 (where cJSON_InitHooks routes
 * all intermediate allocations to PSRAM).
 */
#include "jr_tools/jr_tools_project.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t max_arr;
    size_t max_str;
    size_t max_keys;
    unsigned max_depth;
} project_tier_t;

static const project_tier_t s_tiers[] = {
    {8U, 240U, 20U, 4U},
    {6U, 160U, 14U, 3U},
    {4U, 100U, 10U, 3U},
    {3U,  64U,  8U, 2U},
    {2U,  40U,  6U, 2U},
    {1U,  24U,  4U, 2U},
};

#define JR_PROJECT_MAX_KEY_BYTES   48U
#define JR_PROJECT_STRIP_MAX_DEPTH 16U

static unsigned char ascii_lower(unsigned char c)
{
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
               ? (unsigned char)(c + ('a' - 'A'))
               : c;
}

static bool ascii_streq_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool is_noise_key(const char *key)
{
    static const char *const s_noise[] = {
        "raw",
        "html",
        "raw_html",
        "embedding",
        "embeddings",
        "vector",
        "debug",
        "trace",
        "stack",
        "headers",
        "markdown_full",
    };
    if (key == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_noise) / sizeof(s_noise[0]); ++i) {
        if (ascii_streq_ci(key, s_noise[i])) {
            return true;
        }
    }
    return false;
}

/* Returns the largest prefix length <= max_bytes that does not split a UTF-8
 * multibyte sequence. */
static size_t utf8_safe_cut(const char *s, size_t len, size_t max_bytes)
{
    if (len <= max_bytes) {
        return len;
    }
    size_t cut = max_bytes;
    while (cut > 0U && (((unsigned char)s[cut] & 0xC0U) == 0x80U)) {
        cut--;
    }
    return cut;
}

/* Prints {"result":<value>} directly into out without duplicating value. */
static bool print_wrapped(const cJSON *value, char *out, size_t out_cap)
{
    static const char prefix[] = "{\"result\":";
    const size_t prefix_len = sizeof(prefix) - 1U;
    if (value == NULL || out == NULL || out_cap <= prefix_len + 2U) {
        return false;
    }
    const size_t inner_cap = out_cap - prefix_len - 1U;
    if (inner_cap > (size_t)INT_MAX) {
        return false;
    }
    memcpy(out, prefix, prefix_len);
    /* cJSON_PrintPreallocated takes non-const cJSON * but never mutates it. */
    if (!cJSON_PrintPreallocated((cJSON *)(uintptr_t)value, out + prefix_len,
                                 (int)inner_cap, false)) {
        out[0] = '\0';
        return false;
    }
    const size_t body_len = strlen(out + prefix_len);
    if (prefix_len + body_len + 2U > out_cap) {
        out[0] = '\0';
        return false;
    }
    out[prefix_len + body_len] = '}';
    out[prefix_len + body_len + 1U] = '\0';
    return true;
}

static bool has_noise_key_bounded(const cJSON *node, unsigned depth)
{
    if (node == NULL || depth >= JR_PROJECT_STRIP_MAX_DEPTH) {
        return false;
    }
    if (cJSON_IsObject(node)) {
        for (const cJSON *c = node->child; c != NULL; c = c->next) {
            if (is_noise_key(c->string) ||
                has_noise_key_bounded(c, depth + 1U)) {
                return true;
            }
        }
    } else if (cJSON_IsArray(node)) {
        for (const cJSON *c = node->child; c != NULL; c = c->next) {
            if (has_noise_key_bounded(c, depth + 1U)) {
                return true;
            }
        }
    }
    return false;
}

/* Clones node while dropping noise keys. Returns NULL on OOM or if depth
 * exceeds JR_PROJECT_STRIP_MAX_DEPTH (letting compact tiers handle deep trees). */
static cJSON *clone_strip_noise(const cJSON *node, unsigned depth)
{
    if (node == NULL) {
        return NULL;
    }
    if (cJSON_IsObject(node)) {
        if (depth >= JR_PROJECT_STRIP_MAX_DEPTH) {
            return NULL;
        }
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            return NULL;
        }
        for (const cJSON *c = node->child; c != NULL; c = c->next) {
            if (c->string == NULL || is_noise_key(c->string)) {
                continue;
            }
            cJSON *child = clone_strip_noise(c, depth + 1U);
            if (child == NULL) {
                cJSON_Delete(obj);
                return NULL;
            }
            if (!cJSON_AddItemToObject(obj, c->string, child)) {
                cJSON_Delete(child);
                cJSON_Delete(obj);
                return NULL;
            }
        }
        return obj;
    }
    if (cJSON_IsArray(node)) {
        if (depth >= JR_PROJECT_STRIP_MAX_DEPTH) {
            return NULL;
        }
        cJSON *arr = cJSON_CreateArray();
        if (arr == NULL) {
            return NULL;
        }
        for (const cJSON *c = node->child; c != NULL; c = c->next) {
            cJSON *child = clone_strip_noise(c, depth + 1U);
            if (child == NULL) {
                cJSON_Delete(arr);
                return NULL;
            }
            if (!cJSON_AddItemToArray(arr, child)) {
                cJSON_Delete(child);
                cJSON_Delete(arr);
                return NULL;
            }
        }
        return arr;
    }
    return cJSON_Duplicate(node, false);
}

static cJSON *make_truncated_string(const char *s, size_t max_str)
{
    if (s == NULL) {
        return cJSON_CreateString("");
    }
    const size_t len = strlen(s);
    if (len <= max_str) {
        return cJSON_CreateString(s);
    }
    const size_t cut = utf8_safe_cut(s, len, max_str);
    char stack_buf[256];
    char *buf = stack_buf;
    if (cut + 4U > sizeof(stack_buf)) {
        buf = (char *)malloc(cut + 4U);
        if (buf == NULL) {
            return NULL;
        }
    }
    if (cut > 0U) {
        memcpy(buf, s, cut);
    }
    memcpy(buf + cut, "...", 4U);
    cJSON *item = cJSON_CreateString(buf);
    if (buf != stack_buf) {
        free(buf);
    }
    return item;
}

static void format_bounded_key(const char *src, char *dst, size_t dst_cap)
{
    if (src == NULL || dst_cap == 0U) {
        if (dst_cap > 0U) {
            dst[0] = '\0';
        }
        return;
    }
    const size_t max_bytes = dst_cap - 1U;
    const size_t len = strlen(src);
    const size_t cut = utf8_safe_cut(src, len, max_bytes);
    if (cut > 0U) {
        memcpy(dst, src, cut);
    }
    dst[cut] = '\0';
}

static cJSON *project_node(const cJSON *node, const project_tier_t *tier,
                           unsigned depth)
{
    if (node == NULL) {
        return NULL;
    }
    if (cJSON_IsString(node)) {
        return make_truncated_string(node->valuestring, tier->max_str);
    }
    if (cJSON_IsArray(node)) {
        if (depth >= tier->max_depth) {
            return cJSON_CreateString("[...]");
        }
        cJSON *arr = cJSON_CreateArray();
        if (arr == NULL) {
            return NULL;
        }
        size_t kept = 0U;
        for (const cJSON *c = node->child;
             c != NULL && kept < tier->max_arr; c = c->next) {
            cJSON *child = project_node(c, tier, depth + 1U);
            if (child == NULL) {
                cJSON_Delete(arr);
                return NULL;
            }
            if (!cJSON_AddItemToArray(arr, child)) {
                cJSON_Delete(child);
                cJSON_Delete(arr);
                return NULL;
            }
            kept++;
        }
        return arr;
    }
    if (cJSON_IsObject(node)) {
        if (depth >= tier->max_depth) {
            return cJSON_CreateString("{...}");
        }
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            return NULL;
        }
        size_t kept = 0U;
        for (const cJSON *c = node->child;
             c != NULL && kept < tier->max_keys; c = c->next) {
            if (c->string == NULL || is_noise_key(c->string)) {
                continue;
            }
            char key_buf[JR_PROJECT_MAX_KEY_BYTES + 1U];
            format_bounded_key(c->string, key_buf, sizeof(key_buf));
            if (key_buf[0] == '\0' ||
                cJSON_GetObjectItemCaseSensitive(obj, key_buf) != NULL) {
                continue;
            }
            cJSON *child = project_node(c, tier, depth + 1U);
            if (child == NULL) {
                cJSON_Delete(obj);
                return NULL;
            }
            if (!cJSON_AddItemToObject(obj, key_buf, child)) {
                cJSON_Delete(child);
                cJSON_Delete(obj);
                return NULL;
            }
            kept++;

            if (cJSON_IsArray(c)) {
                const int orig_len = cJSON_GetArraySize(c);
                if (orig_len > 0 &&
                    ((size_t)orig_len > tier->max_arr ||
                     depth + 1U >= tier->max_depth)) {
                    char prefix[JR_PROJECT_MAX_KEY_BYTES - 6U + 1U];
                    char total_key[JR_PROJECT_MAX_KEY_BYTES + 1U];
                    format_bounded_key(c->string, prefix, sizeof(prefix));
                    (void)snprintf(total_key, sizeof(total_key), "%s_total",
                                   prefix);
                    if (cJSON_GetObjectItemCaseSensitive(obj, total_key) == NULL) {
                        if (cJSON_AddNumberToObject(obj, total_key,
                                                    (double)orig_len) == NULL) {
                            cJSON_Delete(obj);
                            return NULL;
                        }
                    }
                }
            }
        }
        return obj;
    }
    return cJSON_Duplicate(node, false);
}

/* Attaches "truncated":true and JR_TOOLS_PROJECT_HINT to a projected root.
 * Arrays and strings are wrapped in an object carrying total / total_chars. */
static cJSON *finalize_projected_root(const cJSON *upstream, cJSON *proj)
{
    if (proj == NULL) {
        return NULL;
    }
    cJSON *root = proj;
    if (cJSON_IsArray(proj)) {
        root = cJSON_CreateObject();
        if (root == NULL) {
            cJSON_Delete(proj);
            return NULL;
        }
        if (!cJSON_AddItemToObject(root, "items", proj)) {
            cJSON_Delete(proj);
            cJSON_Delete(root);
            return NULL;
        }
        if (cJSON_AddNumberToObject(root, "total",
                                    (double)cJSON_GetArraySize(upstream)) == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
    } else if (cJSON_IsString(proj)) {
        root = cJSON_CreateObject();
        if (root == NULL) {
            cJSON_Delete(proj);
            return NULL;
        }
        if (!cJSON_AddItemToObject(root, "text", proj)) {
            cJSON_Delete(proj);
            cJSON_Delete(root);
            return NULL;
        }
        if (cJSON_IsString(upstream) && upstream->valuestring != NULL) {
            if (cJSON_AddNumberToObject(root, "total_chars",
                                        (double)strlen(upstream->valuestring)) == NULL) {
                cJSON_Delete(root);
                return NULL;
            }
        }
    } else if (!cJSON_IsObject(proj)) {
        return proj;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, "truncated");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "hint");
    if (cJSON_AddBoolToObject(root, "truncated", true) == NULL ||
        cJSON_AddStringToObject(root, "hint", JR_TOOLS_PROJECT_HINT) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static size_t utf8_char_len(const unsigned char *p)
{
    const unsigned char c = *p;
    if (c < 0x80U) {
        return 1U;
    }
    if ((c & 0xE0U) == 0xC0U && (p[1] & 0xC0U) == 0x80U) {
        return 2U;
    }
    if ((c & 0xF0U) == 0xE0U && (p[1] & 0xC0U) == 0x80U &&
        (p[2] & 0xC0U) == 0x80U) {
        return 3U;
    }
    if ((c & 0xF8U) == 0xF0U && (p[1] & 0xC0U) == 0x80U &&
        (p[2] & 0xC0U) == 0x80U && (p[3] & 0xC0U) == 0x80U) {
        return 4U;
    }
    return 0U;
}

/* Last-resort bounded wrapper used only when even the tightest structured
 * tier exceeds out_cap. Never splits a JSON escape or UTF-8 sequence. */
static bool fallback_raw_head(const cJSON *upstream, char *out, size_t out_cap)
{
    static const char head[] =
        "{\"result\":{\"truncated\":true,\"hint\":\"" JR_TOOLS_PROJECT_HINT
        "\",\"text\":\"";
    static const char tail[] = "\"}}";
    static const char minimal[] = "{\"result\":{\"truncated\":true}}";

    const size_t hlen = sizeof(head) - 1U;
    const size_t tlen = sizeof(tail) - 1U;

    if (out_cap <= hlen + tlen + 1U) {
        if (out_cap >= sizeof(minimal)) {
            memcpy(out, minimal, sizeof(minimal));
            return true;
        }
        out[0] = '\0';
        return false;
    }

    char *raw = cJSON_PrintUnformatted((cJSON *)(uintptr_t)upstream);
    if (raw == NULL) {
        memcpy(out, minimal, sizeof(minimal));
        return true;
    }

    const size_t max_w = out_cap - tlen - 1U;
    memcpy(out, head, hlen);
    size_t w = hlen;
    const unsigned char *p = (const unsigned char *)raw;
    while (*p != '\0') {
        const unsigned char ch = *p;
        if (ch == '"' || ch == '\\') {
            if (w + 2U > max_w) {
                break;
            }
            out[w++] = '\\';
            out[w++] = (char)ch;
            p++;
        } else if (ch == '\b' || ch == '\f' || ch == '\n' || ch == '\r' ||
                   ch == '\t') {
            if (w + 2U > max_w) {
                break;
            }
            out[w++] = '\\';
            out[w++] = (ch == '\b') ? 'b'
                     : (ch == '\f') ? 'f'
                     : (ch == '\n') ? 'n'
                     : (ch == '\r') ? 'r'
                                    : 't';
            p++;
        } else if (ch < 0x20U) {
            if (w + 6U > max_w) {
                break;
            }
            w += (size_t)snprintf(out + w, out_cap - w, "\\u%04x",
                                  (unsigned)ch);
            p++;
        } else if (ch < 0x80U) {
            if (w + 1U > max_w) {
                break;
            }
            out[w++] = (char)ch;
            p++;
        } else {
            const size_t seq = utf8_char_len(p);
            if (seq == 0U) {
                /* Invalid UTF-8 byte: escape safely as \u00xx. */
                if (w + 6U > max_w) {
                    break;
                }
                w += (size_t)snprintf(out + w, out_cap - w, "\\u%04x",
                                      (unsigned)ch);
                p++;
            } else {
                if (w + seq > max_w) {
                    break;
                }
                memcpy(out + w, p, seq);
                w += seq;
                p += seq;
            }
        }
    }
    memcpy(out + w, tail, tlen);
    out[w + tlen] = '\0';
    cJSON_free(raw);
    return true;
}

bool jr_tools_project_json(const cJSON *upstream, char *out, size_t out_cap,
                           jr_tools_project_info_t *info)
{
    if (info != NULL) {
        info->truncated = false;
        info->used_fallback = false;
        info->tier_index = -1;
    }
    if (out != NULL && out_cap > 0U) {
        out[0] = '\0';
    }
    if (upstream == NULL || out == NULL || out_cap < 30U) {
        return false;
    }

    /* Pass 0: untouched fast path (zero heap duplication). */
    if (print_wrapped(upstream, out, out_cap)) {
        return true;
    }

    /* Pass 1: if noise keys exist, try stripping only noise keys first so
     * clean sibling fields and arrays survive at full fidelity. */
    if (has_noise_key_bounded(upstream, 0U)) {
        cJSON *stripped = finalize_projected_root(
            upstream, clone_strip_noise(upstream, 0U));
        if (stripped != NULL) {
            const bool fit = print_wrapped(stripped, out, out_cap);
            cJSON_Delete(stripped);
            if (fit) {
                if (info != NULL) {
                    info->truncated = true;
                    info->tier_index = 0;
                }
                return true;
            }
        }
    }

    /* Pass 2: progressive structured compaction tiers. */
    const size_t n_tiers = sizeof(s_tiers) / sizeof(s_tiers[0]);
    for (size_t i = 0; i < n_tiers; ++i) {
        cJSON *proj = finalize_projected_root(
            upstream, project_node(upstream, &s_tiers[i], 0U));
        if (proj == NULL) {
            continue;
        }
        const bool fit = print_wrapped(proj, out, out_cap);
        cJSON_Delete(proj);
        if (fit) {
            if (info != NULL) {
                info->truncated = true;
                info->tier_index = (int)(i + 1U);
            }
            return true;
        }
    }

    /* Pass 3: raw string-head escape wrapper (last resort). */
    if (fallback_raw_head(upstream, out, out_cap)) {
        if (info != NULL) {
            info->truncated = true;
            info->used_fallback = true;
            info->tier_index = (int)(n_tiers + 1U);
        }
        return true;
    }
    return false;
}
