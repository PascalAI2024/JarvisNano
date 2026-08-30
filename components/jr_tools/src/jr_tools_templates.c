/* SPDX-License-Identifier: Apache-2.0 */
#include "jr_tools/jr_tools.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

typedef struct {
    const char *name;
    const char *arg;
    size_t max_len;
    bool optional;
    const char *prefix;
    const char *suffix;
} template_desc_t;

/* These strings are the complete executable vocabulary. There is no generic
 * `code` tool and no path that copies model text outside a quoted literal. */
static const template_desc_t s_templates[] = {
    {"recall_memory", "query", 255U, false,
     "return await jarvis.memory.search({query:", ",area:\"all\",limit:5})"},
    {"remember", "note", 47U, false,
     "const b=", ";const h=await jarvis.utils.hash(b);"
     "return await jarvis.memory.capture({type:\"note\","
     "title:\"JarvisNano \"+h.slice(0,12),body:b,"
     "tags:[\"jarvisnano\",\"voice\"],source:\"jarvisnano\","
     "confidence:\"confirmed\"})"},
    {"current_time", "timezone", 79U, true,
     "return await jarvis.time(", ")"},
    {"search_tools", "query", 191U, false,
     "return await jarvis.meta.search(",
     ",{limit:3,detail:\"compact\"})"},
};

static const template_desc_t *find_template(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_templates) / sizeof(s_templates[0]); ++i) {
        if (strcmp(name, s_templates[i].name) == 0) {
            return &s_templates[i];
        }
    }
    return NULL;
}


static bool valid_panel_note(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (isalnum(*p) || *p == ' ' || *p == '-' || *p == '_' ||
            *p == '.' || *p == ':' || *p == '/' || *p == '?' ||
            *p == '!' || *p == '+' || *p == ',' || *p == '\'' ||
            *p == '(' || *p == ')' || *p == '&') {
            continue;
        }
        return false;
    }
    return true;
}

bool jr_tools_build_request_id(uint32_t boot_nonce, uint32_t session_gen,
                               uint32_t call_id, const char *call_id_text,
                               char *out, size_t out_cap)
{
    if (out == NULL || out_cap < 29U || boot_nonce == 0U ||
        (call_id == 0U && (call_id_text == NULL || call_id_text[0] == '\0'))) {
        return false;
    }
    uint32_t stable = call_id;
    if (stable == 0U) {
        stable = 2166136261U;
        for (const unsigned char *p = (const unsigned char *)call_id_text;
             *p != '\0'; ++p) {
            stable = (stable ^ *p) * 16777619U;
        }
        if (stable == 0U) stable = 1U;
    }
    int prefix = snprintf(out, out_cap, "g:%08lx:%08lx:%08lx",
                          (unsigned long)boot_nonce,
                          (unsigned long)session_gen,
                          (unsigned long)stable);
    if (prefix < 0 || (size_t)prefix >= out_cap) {
        if (out_cap > 0U) out[0] = '\0';
        return false;
    }
    if (call_id_text == NULL || call_id_text[0] == '\0') {
        return true;
    }

    size_t used = (size_t)prefix;
    if (used + 1U >= out_cap || used + 1U >= JR_TOOLS_REQUEST_ID_CAP) {
        return true;
    }
    out[used++] = ':';
    for (const unsigned char *p = (const unsigned char *)call_id_text;
         *p != '\0' && used + 1U < out_cap &&
         used + 1U < JR_TOOLS_REQUEST_ID_CAP; ++p) {
        out[used++] = (isalnum(*p) || *p == '.' || *p == '_' ||
                       *p == ':' || *p == '-') ? (char)*p : '_';
    }
    out[used] = '\0';
    return true;
}

static jr_tool_template_status_t json_string_literal(const char *value,
                                                      char **out)
{
    cJSON *item = cJSON_CreateString(value);
    if (item == NULL) {
        return JR_TOOL_TEMPLATE_INTERNAL_ERROR;
    }
    *out = cJSON_PrintUnformatted(item);
    cJSON_Delete(item);
    return *out != NULL ? JR_TOOL_TEMPLATE_OK : JR_TOOL_TEMPLATE_INTERNAL_ERROR;
}

jr_tool_template_status_t jr_tools_build_code(const char *name,
                                               const char *args_json,
                                               char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0U) {
        return JR_TOOL_TEMPLATE_INVALID_ARGS;
    }
    out[0] = '\0';
    if (name != NULL && strcmp(name, "execute_tool") == 0) {
        const char *json =
            (args_json == NULL || args_json[0] == '\0') ? "{}" : args_json;
        const char *parse_end = NULL;
        cJSON *args = cJSON_ParseWithOpts(json, &parse_end, true);
        cJSON *tool = cJSON_GetObjectItemCaseSensitive(args, "tool");
        cJSON *nested_text =
            cJSON_GetObjectItemCaseSensitive(args, "args_json");
        bool valid = args != NULL && cJSON_IsObject(args) &&
            cJSON_IsString(tool) && tool->valuestring != NULL &&
            cJSON_IsString(nested_text) && nested_text->valuestring != NULL;
        unsigned fields = 0U;
        for (const cJSON *item = valid ? args->child : NULL;
             item != NULL; item = item->next) {
            if (item->string == NULL ||
                (strcmp(item->string, "tool") != 0 &&
                 strcmp(item->string, "args_json") != 0)) {
                valid = false;
            }
            fields++;
        }
        size_t tool_len = valid
            ? strnlen(tool->valuestring, JR_TOOLS_NAME_CAP) : 0U;
        for (size_t i = 0; valid && i < tool_len; ++i) {
            unsigned char ch = (unsigned char)tool->valuestring[i];
            if (!isalnum(ch) && ch != '.' && ch != '_' && ch != '-') {
                valid = false;
            }
        }
        cJSON *nested = valid
            ? cJSON_ParseWithOpts(nested_text->valuestring, NULL, true) : NULL;
        valid = valid && fields == 2U && tool_len > 0U &&
            tool_len < JR_TOOLS_NAME_CAP && cJSON_IsObject(nested);
        cJSON_Delete(nested);
        cJSON_Delete(args);
        if (!valid) {
            return JR_TOOL_TEMPLATE_INVALID_ARGS;
        }
        int written = snprintf(out, out_cap, "%s",
            "return {error:\"execute_tool requires typed device gateway\"}");
        if (written < 0 || (size_t)written >= out_cap) {
            out[0] = '\0';
            return JR_TOOL_TEMPLATE_TOO_LARGE;
        }
        return JR_TOOL_TEMPLATE_OK;
    }

    const template_desc_t *desc = find_template(name);
    if (desc == NULL) {
        return JR_TOOL_TEMPLATE_UNKNOWN_TOOL;
    }

    const char *json = (args_json == NULL || args_json[0] == '\0') ? "{}" : args_json;
    if (strnlen(json, JR_TOOLS_ARGS_CAP) >= JR_TOOLS_ARGS_CAP) {
        return JR_TOOL_TEMPLATE_TOO_LARGE;
    }
    const char *parse_end = NULL;
    cJSON *args = cJSON_ParseWithOpts(json, &parse_end, true);
    if (args == NULL || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        return JR_TOOL_TEMPLATE_INVALID_ARGS;
    }

    cJSON *arg = cJSON_GetObjectItemCaseSensitive(args, desc->arg);
    bool saw_arg = false;
    for (const cJSON *item = args->child; item != NULL; item = item->next) {
        if (item->string == NULL || strcmp(item->string, desc->arg) != 0 ||
            saw_arg) {
            cJSON_Delete(args);
            return JR_TOOL_TEMPLATE_INVALID_ARGS;
        }
        saw_arg = true;
    }
    const char *value = NULL;
    if (arg != NULL) {
        if (!cJSON_IsString(arg) || arg->valuestring == NULL) {
            cJSON_Delete(args);
            return JR_TOOL_TEMPLATE_INVALID_ARGS;
        }
        value = arg->valuestring;
    } else if (!desc->optional) {
        cJSON_Delete(args);
        return JR_TOOL_TEMPLATE_INVALID_ARGS;
    }

    /* The legacy /act path needs an explicit timezone because jarvis.time()
     * with no argument lists zones rather than returning the current time. The
     * typed gateway applies its own configured device timezone to the original
     * empty args object. */
    if (value == NULL || value[0] == '\0') {
        if (strcmp(desc->name, "current_time") == 0) {
            value = "America/New_York";
        } else if (desc->optional) {
            int n = snprintf(out, out_cap, "%s%s", desc->prefix, desc->suffix);
            cJSON_Delete(args);
            return (n >= 0 && (size_t)n < out_cap)
                       ? JR_TOOL_TEMPLATE_OK : JR_TOOL_TEMPLATE_TOO_LARGE;
        } else {
            cJSON_Delete(args);
            return JR_TOOL_TEMPLATE_INVALID_ARGS;
        }
    }

    size_t value_len = strnlen(value, desc->max_len + 1U);
    if (value_len == 0U || value_len > desc->max_len ||
        (strcmp(desc->name, "remember") == 0 && !valid_panel_note(value))) {
        cJSON_Delete(args);
        return JR_TOOL_TEMPLATE_INVALID_ARGS;
    }

    /* cJSON produces a JSON double-quoted string literal. JSON string syntax
     * is also valid JavaScript string syntax, so quotes, slashes, and control
     * characters stay data rather than executable source. */
    char *literal = NULL;
    jr_tool_template_status_t status = json_string_literal(value, &literal);
    if (status == JR_TOOL_TEMPLATE_OK) {
        int n = snprintf(out, out_cap, "%s%s%s", desc->prefix, literal,
                         desc->suffix);
        if (n < 0 || (size_t)n >= out_cap) {
            out[0] = '\0';
            status = JR_TOOL_TEMPLATE_TOO_LARGE;
        }
    }
    free(literal);
    cJSON_Delete(args);
    return status;
}
