/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_transport/gemini_live.c — L2 Gemini Live transport (builders + framer +
 * parser). ESP-IDF-free: depends only on jr_ports headers, jr_core types, libc,
 * and cJSON (vendored on host / IDF `json` on device). The SAME translation is
 * exercised by the host Unity suite (host/test_transport.c) and compiled into
 * the device build; only the byte-level ws_transport adapter differs per target.
 *
 * The load-bearing correctness win is the FRAMER (jr_gemini_send_frame /
 * jr_gemini_flush): a would-block / 0-byte / partial send buffers-or-drops
 * (drop-newest, hard-bounded) and KEEPS the connection. It never aborts — that
 * class of bug (a transient TCP would-block sinking the whole WebSocket) is
 * structurally dead here (spec §5.6).
 */
#include "jr_transport/gemini_live.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "cJSON.h"

/* ======================================================================== *
 *  base64 (portable, endian-safe)                                          *
 * ======================================================================== */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const uint8_t *data, size_t len)
{
    size_t olen = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(olen + 1);
    if (!out) {
        return NULL;
    }
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) |
                     (uint32_t)data[i + 2];
        i += 3;
        out[o++] = B64[(n >> 18) & 63];
        out[o++] = B64[(n >> 12) & 63];
        out[o++] = B64[(n >> 6) & 63];
        out[o++] = B64[n & 63];
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[o++] = B64[(n >> 18) & 63];
        out[o++] = B64[(n >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = B64[(n >> 18) & 63];
        out[o++] = B64[(n >> 12) & 63];
        out[o++] = B64[(n >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') { return c - 'A'; }
    if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
    if (c >= '0' && c <= '9') { return c - '0' + 52; }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

/* Decode into a fresh byte buffer; *outlen set to byte count. Whitespace and
 * padding are skipped. Returns NULL on OOM. */
static uint8_t *b64_decode(const char *s, size_t *outlen)
{
    size_t n = 0;
    for (const char *p = s; *p; p++) {
        if (b64_val((unsigned char)*p) >= 0) { n++; }
    }
    size_t cap = (n / 4) * 3 + 3;
    uint8_t *out = (uint8_t *)malloc(cap ? cap : 1);
    if (!out) {
        return NULL;
    }
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (const char *p = s; *p; p++) {
        int v = b64_val((unsigned char)*p);
        if (v < 0) { continue; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    *outlen = o;
    return out;
}

/* Stable non-zero hash so a toolCall id string maps to the L3's uint32 call_id,
 * and a later toolCallCancellation for the SAME id yields the SAME call_id. */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s) { return 0; }
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static char *dup_cstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) { memcpy(d, s, n); }
    return d;
}

/* ======================================================================== *
 *  BUILDERS                                                                 *
 * ======================================================================== */
char *jr_gemini_build_setup(const jr_gemini_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { return NULL; }
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON_AddStringToObject(setup, "model",
                            (cfg && cfg->model) ? cfg->model : JR_GEMINI_MODEL_PRIMARY);

    /* generationConfig.responseModalities + thinkingConfig.thinkingLevel
     * (NESTED under thinkingConfig — the v4 flat-field bug: a flat thinkingLevel
     * is silently ignored by the server). */
    cJSON *gc = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *rm = cJSON_AddArrayToObject(gc, "responseModalities");
    cJSON_AddItemToArray(rm, cJSON_CreateString("AUDIO"));
    cJSON *tcfg = cJSON_AddObjectToObject(gc, "thinkingConfig");
    cJSON_AddStringToObject(tcfg, "thinkingLevel",
                            (cfg && cfg->thinking_level) ? cfg->thinking_level : "low");

    /* speechConfig — output-language anchor + fixed voice. Without a
     * languageCode the native-audio model drifts into arbitrary languages
     * (observed on hardware); the strong systemInstruction is the primary
     * lever, this is the belt. languageCode is a sibling of voiceConfig inside
     * generationConfig.speechConfig. Emitted only when configured. */
    if (cfg && (cfg->voice_name || cfg->language_code)) {
        cJSON *sc = cJSON_AddObjectToObject(gc, "speechConfig");
        if (cfg->language_code) {
            cJSON_AddStringToObject(sc, "languageCode", cfg->language_code);
        }
        if (cfg->voice_name) {
            cJSON *vc = cJSON_AddObjectToObject(sc, "voiceConfig");
            cJSON *pv = cJSON_AddObjectToObject(vc, "prebuiltVoiceConfig");
            cJSON_AddStringToObject(pv, "voiceName", cfg->voice_name);
        }
    }

    /* realtimeInputConfig — manual PTT vs server VAD. Manual sets
     * automaticActivityDetection.disabled=true + activityHandling=NO_INTERRUPTION
     * so a client activityStart can't self-cancel the model's reply. */
    cJSON *ric = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = cJSON_AddObjectToObject(ric, "automaticActivityDetection");
    if (cfg && cfg->vad_mode == JR_VAD_SERVER) {
        cJSON_AddBoolToObject(aad, "disabled", false);
        /* HIGH onset sensitivity: server-side barge is the primary interrupt
         * path (docs: START_OF_ACTIVITY_INTERRUPTS fires on detected user
         * speech DURING model audio). Our AEC keeps the echo residual ~0.5%
         * on the 1.75C, so eager onset detection favors catching the owner's
         * talk-over without self-triggering on the speaker loop. */
        cJSON_AddStringToObject(aad, "startOfSpeechSensitivity",
                                "START_SENSITIVITY_HIGH");
        cJSON_AddStringToObject(ric, "activityHandling", "START_OF_ACTIVITY_INTERRUPTS");
    } else {
        cJSON_AddBoolToObject(aad, "disabled", true);
        cJSON_AddStringToObject(ric, "activityHandling", "NO_INTERRUPTION");
    }

    /* tools — functionDeclarations + googleSearch COEXISTING. */
    cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
    if (cfg && cfg->google_search) {
        cJSON *gs = cJSON_CreateObject();
        cJSON_AddItemToObject(gs, "googleSearch", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, gs);
    }
    if (cfg && cfg->fns && cfg->fn_count) {
        cJSON *fdtool = cJSON_CreateObject();
        cJSON *fdecls = cJSON_AddArrayToObject(fdtool, "functionDeclarations");
        for (size_t i = 0; i < cfg->fn_count; i++) {
            const jr_gemini_fn_decl_t *d = &cfg->fns[i];
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", d->name ? d->name : "");
            cJSON_AddStringToObject(fn, "description", d->description ? d->description : "");
            cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
            cJSON_AddStringToObject(params, "type", "object");
            cJSON *props = cJSON_AddObjectToObject(params, "properties");
            /* "required" is created LAZILY and always AFTER "properties", which
             * is what keeps a legacy single-string declaration byte-identical to
             * the pre-typed-params emitter (a host test asserts that exactly). */
            cJSON *reqd = NULL;

            /* (a) legacy single-string arg — emitted first, unchanged. */
            if (d->arg_name) {
                cJSON *p = cJSON_AddObjectToObject(props, d->arg_name);
                cJSON_AddStringToObject(p, "type", "string");
                cJSON_AddStringToObject(p, "description", d->arg_desc ? d->arg_desc : "");
                reqd = cJSON_AddArrayToObject(params, "required");
                cJSON_AddItemToArray(reqd, cJSON_CreateString(d->arg_name));
            }

            /* (b) typed params[] — mixed types incl. STRING_ARRAY. A tool with
             * param_count == 0 (every legacy declaration) skips this entirely. */
            const size_t np = d->param_count;
            if (np > JR_GEMINI_MAX_FN_PARAMS) {
                /* NO SILENT TRUNCATION. Clamping here shipped a schema missing
                 * its last property, invisible until Gemini mis-called the
                 * tool. JR_GEMINI_PARAM_COUNT() catches this at compile time;
                 * a table that dodged the macro fails loudly HERE instead —
                 * there is no logger in this ESP-IDF-free layer, so the only
                 * loud channel is the NULL the caller already checks. */
                cJSON_Delete(fn);
                cJSON_Delete(fdtool);
                cJSON_Delete(root);
                return NULL;
            }
            for (size_t k = 0; k < np; k++) {
                const jr_gemini_fn_param_t *pp = &d->params[k];
                if (!pp->name) { continue; }
                cJSON *p = cJSON_AddObjectToObject(props, pp->name);
                if (!p) { continue; }
                const char *tname;
                switch (pp->type) {
                case JR_GEMINI_PT_STRING_ARRAY: tname = "array";   break;
                case JR_GEMINI_PT_INTEGER:      tname = "integer"; break;
                case JR_GEMINI_PT_BOOLEAN:      tname = "boolean"; break;
                case JR_GEMINI_PT_STRING:
                default:                        tname = "string";  break;
                }
                cJSON_AddStringToObject(p, "type", tname);
                cJSON_AddStringToObject(p, "description",
                                        pp->description ? pp->description : "");
                if (pp->type == JR_GEMINI_PT_STRING_ARRAY) {
                    /* An array property is INVALID to Gemini without "items";
                     * the whole reason ask_user could not be declared before. */
                    cJSON *items = cJSON_AddObjectToObject(p, "items");
                    cJSON_AddStringToObject(items, "type", "string");
                }
                if (pp->required) {
                    if (!reqd) { reqd = cJSON_AddArrayToObject(params, "required"); }
                    cJSON_AddItemToArray(reqd, cJSON_CreateString(pp->name));
                }
            }
            cJSON_AddItemToArray(fdecls, fn);
        }
        cJSON_AddItemToArray(tools, fdtool);
    }

    if (cfg && cfg->input_transcription) {
        cJSON_AddItemToObject(setup, "inputAudioTranscription", cJSON_CreateObject());
    }
    if (cfg && cfg->output_transcription) {
        cJSON_AddItemToObject(setup, "outputAudioTranscription", cJSON_CreateObject());
    }

    /* proactivity.proactiveAudio — let the model decline to answer ambient or
     * out-of-context audio and stay silent when no real request was made.
     * Directly suppresses the phantom self-triggered turns the device's own
     * speech tail was provoking (setup sibling of generationConfig). */
    if (cfg && cfg->proactive_audio) {
        cJSON *pro = cJSON_AddObjectToObject(setup, "proactivity");
        cJSON_AddBoolToObject(pro, "proactiveAudio", true);
    }

    if (cfg && cfg->system_instruction) {
        cJSON *si = cJSON_AddObjectToObject(setup, "systemInstruction");
        cJSON *parts = cJSON_AddArrayToObject(si, "parts");
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", cfg->system_instruction);
        cJSON_AddItemToArray(parts, part);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

char *jr_gemini_build_activity_start(void)
{
    return dup_cstr("{\"realtimeInput\":{\"activityStart\":{}}}");
}
char *jr_gemini_build_activity_end(void)
{
    return dup_cstr("{\"realtimeInput\":{\"activityEnd\":{}}}");
}
char *jr_gemini_build_audio_stream_end(void)
{
    /* AUTO-VAD ONLY. The client's send_control refuses to emit this in manual
     * mode — this builder exists for the auto path and the keepalive. */
    return dup_cstr("{\"realtimeInput\":{\"audioStreamEnd\":true}}");
}

char *jr_gemini_build_text_turn(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { return NULL; }
    cJSON *cc = cJSON_AddObjectToObject(root, "clientContent");
    cJSON *turns = cJSON_AddArrayToObject(cc, "turns");
    cJSON *turn = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "role", "user");
    cJSON *parts = cJSON_AddArrayToObject(turn, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text ? text : "");
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToArray(turns, turn);
    cJSON_AddBoolToObject(cc, "turnComplete", true);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

char *jr_gemini_build_tool_response(const char *call_id, const char *name,
                                    const char *response_json)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { return NULL; }
    cJSON *tr = cJSON_AddObjectToObject(root, "toolResponse");
    cJSON *responses = cJSON_AddArrayToObject(tr, "functionResponses");
    cJSON *fr = cJSON_CreateObject();
    cJSON_AddStringToObject(fr, "id", call_id ? call_id : "");
    cJSON_AddStringToObject(fr, "name", name ? name : "");

    cJSON *response = response_json ? cJSON_Parse(response_json) : NULL;
    if (!cJSON_IsObject(response)) {
        if (response) { cJSON_Delete(response); }
        response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "error",
                                response_json ? response_json : "empty tool response");
    }
    cJSON_AddItemToObject(fr, "response", response);
    cJSON_AddItemToArray(responses, fr);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

char *jr_gemini_build_ask_user_response(const char *call_id, const char *answer)
{
    /* Build the payload through cJSON so a quote/backslash/newline inside a
     * chosen option is escaped rather than splicing raw text into a frame. */
    cJSON *resp = cJSON_CreateObject();
    if (!resp) { return NULL; }
    if (!cJSON_AddStringToObject(resp, "answer", answer ? answer : "")) {
        cJSON_Delete(resp);
        return NULL;
    }
    char *payload = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!payload) { return NULL; }

    /* Delegate so an answered question travels the IDENTICAL toolResponse path
     * as every other tool — one emitter, one shape. */
    char *frame = jr_gemini_build_tool_response(call_id, JR_GEMINI_ASK_USER_TOOL,
                                                payload);
    free(payload);
    return frame;
}

char *jr_gemini_build_audio_chunk(const jr_pcm_t *pcm, size_t samples, uint32_t rate)
{
    size_t nbytes = samples * sizeof(int16_t);
    uint8_t *le = (uint8_t *)malloc(nbytes ? nbytes : 1);
    if (!le) { return NULL; }
    for (size_t i = 0; i < samples; i++) {
        uint16_t s = (uint16_t)pcm[i];
        le[2 * i]     = (uint8_t)(s & 0xff);
        le[2 * i + 1] = (uint8_t)((s >> 8) & 0xff);
    }
    char *b64 = b64_encode(le, nbytes);
    free(le);
    if (!b64) { return NULL; }

    cJSON *root = cJSON_CreateObject();
    if (!root) { free(b64); return NULL; }
    cJSON *ri = cJSON_AddObjectToObject(root, "realtimeInput");
    cJSON *audio = cJSON_AddObjectToObject(ri, "audio");
    char mime[40];
    snprintf(mime, sizeof(mime), "audio/pcm;rate=%u", (unsigned)rate);
    cJSON_AddStringToObject(audio, "mimeType", mime);
    cJSON_AddStringToObject(audio, "data", b64);
    free(b64);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ======================================================================== *
 *  PARSER                                                                   *
 * ======================================================================== */
static cJSON *obj_compat(const cJSON *parent, const char *a, const char *b)
{
    cJSON *x = cJSON_GetObjectItemCaseSensitive(parent, a);
    if (x) { return x; }
    return cJSON_GetObjectItemCaseSensitive(parent, b);
}

/* Decode a protobuf Duration (JSON canonical string "60s"/"1.500s", a bare
 * number of seconds, or an object {seconds, nanos}) to whole seconds. */
static uint32_t parse_duration_seconds(const cJSON *d)
{
    if (!d) { return 0; }
    if (cJSON_IsString(d) && d->valuestring) {
        double sec = strtod(d->valuestring, NULL);   /* stops at the 's' suffix */
        if (sec < 0) { sec = 0; }
        return (uint32_t)(sec + 0.5);
    }
    if (cJSON_IsNumber(d)) {
        double sec = d->valuedouble;
        if (sec < 0) { sec = 0; }
        return (uint32_t)(sec + 0.5);
    }
    if (cJSON_IsObject(d)) {
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(d, "seconds");
        const cJSON *nanos = cJSON_GetObjectItemCaseSensitive(d, "nanos");
        double sec = 0;
        if (cJSON_IsNumber(s)) { sec = s->valuedouble; }
        else if (cJSON_IsString(s) && s->valuestring) { sec = strtod(s->valuestring, NULL); }
        if (cJSON_IsNumber(nanos)) { sec += nanos->valuedouble / 1e9; }
        if (sec < 0) { sec = 0; }
        return (uint32_t)(sec + 0.5);
    }
    return 0;
}

static jr_gemini_error_kind_t classify_error(int code, const char *msg)
{
    if (code == 429) { return JR_GEMINI_ERRK_QUOTA; }
    if (code == 401 || code == 403) { return JR_GEMINI_ERRK_AUTH; }
    if (code == 400) { return JR_GEMINI_ERRK_PROTOCOL; }
    if (msg) {
        if (strstr(msg, "quota") || strstr(msg, "Quota") ||
            strstr(msg, "RESOURCE_EXHAUSTED")) {
            return JR_GEMINI_ERRK_QUOTA;
        }
        if (strstr(msg, "UNAUTHENTICATED") || strstr(msg, "PERMISSION_DENIED") ||
            strstr(msg, "API key")) {
            return JR_GEMINI_ERRK_AUTH;
        }
        if (strstr(msg, "INVALID_ARGUMENT")) {
            return JR_GEMINI_ERRK_PROTOCOL;
        }
    }
    if (code >= 500 && code <= 599) { return JR_GEMINI_ERRK_TRANSIENT; }
    return code ? JR_GEMINI_ERRK_TRANSIENT : JR_GEMINI_ERRK_UNKNOWN;
}

static jr_gemini_event_t *push_ev(jr_gemini_parse_t *out, jr_gemini_event_kind_t k)
{
    if (out->count >= JR_GEMINI_MAX_EVENTS) { return NULL; }
    jr_gemini_event_t *e = &out->events[out->count++];
    memset(e, 0, sizeof(*e));
    e->kind = k;
    return e;
}

static void parse_audio_part(jr_gemini_parse_t *out, const cJSON *blob)
{
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(blob, "data");
    if (!cJSON_IsString(data) || !data->valuestring) { return; }
    size_t nbytes = 0;
    uint8_t *bytes = b64_decode(data->valuestring, &nbytes);
    if (!bytes) { return; }
    size_t nsamp = nbytes / 2;
    jr_pcm_t *pcm = (jr_pcm_t *)malloc((nsamp ? nsamp : 1) * sizeof(jr_pcm_t));
    if (!pcm) { free(bytes); return; }
    for (size_t i = 0; i < nsamp; i++) {
        uint16_t s = (uint16_t)bytes[2 * i] | ((uint16_t)bytes[2 * i + 1] << 8);
        pcm[i] = (jr_pcm_t)s;
    }
    free(bytes);
    uint32_t rate = JR_GEMINI_RX_RATE;
    const cJSON *mime = cJSON_GetObjectItemCaseSensitive(blob, "mimeType");
    if (cJSON_IsString(mime) && mime->valuestring) {
        const char *r = strstr(mime->valuestring, "rate=");
        if (r) {
            unsigned long v = strtoul(r + 5, NULL, 10);
            if (v) { rate = (uint32_t)v; }
        }
    }
    jr_gemini_event_t *e = push_ev(out, JR_GEV_AUDIO_CHUNK);
    if (!e) { free(pcm); return; }
    e->pcm = pcm;
    e->pcm_len = nsamp;
    e->sample_rate = rate;
}

bool jr_gemini_parse(const char *json, jr_gemini_parse_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (!root) { return false; }
    out->_root = root;

    /* setupComplete */
    if (cJSON_GetObjectItemCaseSensitive(root, "setupComplete")) {
        push_ev(out, JR_GEV_SETUP_COMPLETE);
        return true;
    }

    /* serverContent — may demux into several events */
    cJSON *svc = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (svc) {
        cJSON *intr = cJSON_GetObjectItemCaseSensitive(svc, "interrupted");
        if (cJSON_IsTrue(intr)) {
            push_ev(out, JR_GEV_INTERRUPTED);
        }
        /* outputAudioTranscription: a running text transcript of the model's
         * own spoken audio. Surfaced as a TEXT event so the owner can log what
         * JARVIS actually said (proof of output language). */
        cJSON *otr = cJSON_GetObjectItemCaseSensitive(svc, "outputTranscription");
        if (otr) {
            cJSON *ot = cJSON_GetObjectItemCaseSensitive(otr, "text");
            if (cJSON_IsString(ot) && ot->valuestring && ot->valuestring[0]) {
                jr_gemini_event_t *e = push_ev(out, JR_GEV_TEXT);
                if (e) { e->text = ot->valuestring; }
            }
        }
        cJSON *turn = cJSON_GetObjectItemCaseSensitive(svc, "modelTurn");
        if (turn) {
            cJSON *parts = cJSON_GetObjectItemCaseSensitive(turn, "parts");
            cJSON *part = NULL;
            cJSON_ArrayForEach(part, parts) {
                cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
                if (cJSON_IsString(text) && text->valuestring) {
                    jr_gemini_event_t *e = push_ev(out, JR_GEV_TEXT);
                    if (e) { e->text = text->valuestring; }
                }
                cJSON *blob = obj_compat(part, "inlineData", "inline_data");
                if (blob) { parse_audio_part(out, blob); }

                /* Some Live API revisions wrap audio blobs in mediaChunks
                 * instead of inlineData. Accept both the documented array
                 * shape and the single-object shape, as the proven v4 parser
                 * does, so a server-side envelope change cannot mute replies. */
                cJSON *chunks = cJSON_GetObjectItemCaseSensitive(part, "mediaChunks");
                if (cJSON_IsArray(chunks)) {
                    cJSON *chunk = NULL;
                    cJSON_ArrayForEach(chunk, chunks) {
                        parse_audio_part(out, chunk);
                    }
                } else if (cJSON_IsObject(chunks)) {
                    parse_audio_part(out, chunks);
                }
            }
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(svc, "turnComplete"))) {
            push_ev(out, JR_GEV_TURN_COMPLETE);
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(svc, "generationComplete"))) {
            push_ev(out, JR_GEV_GENERATION_COMPLETE);
        }
        if (out->count == 0) {
            push_ev(out, JR_GEV_UNKNOWN);
        }
        return true;
    }

    /* sessionResumptionUpdate — heartbeat + (usable) handle */
    cJSON *sru = cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate");
    if (sru) {
        cJSON *nh = cJSON_GetObjectItemCaseSensitive(sru, "newHandle");
        cJSON *res = cJSON_GetObjectItemCaseSensitive(sru, "resumable");
        jr_gemini_event_t *e = push_ev(out, JR_GEV_RESUMPTION_UPDATE);
        if (e) {
            e->resumable = cJSON_IsTrue(res);
            if (cJSON_IsString(nh) && nh->valuestring) { e->handle = nh->valuestring; }
            /* Only a resumable, non-empty handle is usable → nonzero token. */
            e->resumption_token =
                (e->resumable && e->handle && e->handle[0]) ? fnv1a(e->handle) : 0u;
        }
        return true;
    }

    /* toolCall — one event per functionCall */
    cJSON *tool = cJSON_GetObjectItemCaseSensitive(root, "toolCall");
    if (tool) {
        cJSON *fcs = cJSON_GetObjectItemCaseSensitive(tool, "functionCalls");
        cJSON *fc = NULL;
        cJSON_ArrayForEach(fc, fcs) {
            jr_gemini_event_t *e = push_ev(out, JR_GEV_TOOL_CALL);
            if (!e) { break; }
            cJSON *name = cJSON_GetObjectItemCaseSensitive(fc, "name");
            cJSON *id = cJSON_GetObjectItemCaseSensitive(fc, "id");
            cJSON *args = cJSON_GetObjectItemCaseSensitive(fc, "args");
            if (cJSON_IsString(name)) { e->tool_name = name->valuestring; }
            const char *idstr = cJSON_IsString(id) ? id->valuestring : NULL;
            e->call_id_text = idstr;
            e->call_id = fnv1a(idstr ? idstr : (e->tool_name ? e->tool_name : ""));
            if (args) {
                e->tool_args = cJSON_PrintUnformatted(args); /* owned; freed in _free */
                /* Retain the live node too, so a caller reads a typed argument
                 * (ask_user's options[]) without re-parsing. Borrowed from
                 * out->_root — dies with jr_gemini_parse_free(). */
                e->tool_args_node = args;
            }
        }
        if (out->count == 0) { push_ev(out, JR_GEV_UNKNOWN); }
        return true;
    }

    /* toolCallCancellation — one event per cancelled id */
    cJSON *tcc = obj_compat(root, "toolCallCancellation", "tool_call_cancellation");
    if (tcc) {
        cJSON *ids = cJSON_GetObjectItemCaseSensitive(tcc, "ids");
        cJSON *id = NULL;
        cJSON_ArrayForEach(id, ids) {
            jr_gemini_event_t *e = push_ev(out, JR_GEV_TOOL_CANCEL);
            if (!e) { break; }
            if (cJSON_IsString(id)) {
                e->call_id_text = id->valuestring;
                e->call_id = fnv1a(id->valuestring);
            }
        }
        if (out->count == 0) { push_ev(out, JR_GEV_TOOL_CANCEL); }
        return true;
    }

    /* goAway — timeLeft is a protobuf Duration */
    cJSON *ga = cJSON_GetObjectItemCaseSensitive(root, "goAway");
    if (ga) {
        jr_gemini_event_t *e = push_ev(out, JR_GEV_GO_AWAY);
        if (e) {
            e->go_away_seconds =
                parse_duration_seconds(cJSON_GetObjectItemCaseSensitive(ga, "timeLeft"));
        }
        return true;
    }

    /* error */
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (err) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(err, "code");
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        jr_gemini_event_t *e = push_ev(out, JR_GEV_ERROR);
        if (e) {
            e->code = cJSON_IsNumber(code) ? (int)code->valuedouble : 0;
            e->message = cJSON_IsString(msg) ? msg->valuestring : NULL;
            e->error_kind = classify_error(e->code, e->message);
        }
        return true;
    }

    push_ev(out, JR_GEV_UNKNOWN);
    return true;
}

void jr_gemini_parse_free(jr_gemini_parse_t *out)
{
    if (!out) { return; }
    for (size_t i = 0; i < out->count; i++) {
        jr_gemini_event_t *e = &out->events[i];
        if (e->kind == JR_GEV_AUDIO_CHUNK && e->pcm) {
            free(e->pcm);
            e->pcm = NULL;
        }
        if (e->kind == JR_GEV_TOOL_CALL && e->tool_args) {
            free((void *)e->tool_args);   /* cJSON_PrintUnformatted result */
            e->tool_args = NULL;
        }
        /* Borrowed from _root (never owned) — just drop the dangling alias. */
        e->tool_args_node = NULL;
    }
    if (out->_root) {
        cJSON_Delete((cJSON *)out->_root);
        out->_root = NULL;
    }
    out->count = 0;
}

/* ---- typed argument readers over the retained args node ------------------ */
const char *jr_gemini_tool_arg_string(const jr_gemini_event_t *ev, const char *key)
{
    if (!ev || !key || !ev->tool_args_node) { return NULL; }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(
        (const cJSON *)ev->tool_args_node, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

size_t jr_gemini_tool_arg_string_array(const jr_gemini_event_t *ev, const char *key,
                                       const char **out, size_t max)
{
    if (!ev || !key || !out || max == 0 || !ev->tool_args_node) { return 0; }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(
        (const cJSON *)ev->tool_args_node, key);
    if (!cJSON_IsArray(arr)) { return 0; }
    size_t n = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (n >= max) { break; }
        /* Skip a non-string element rather than aborting: a malformed option in
         * an otherwise usable list must not cost the user the whole question. */
        if (cJSON_IsString(it) && it->valuestring) {
            out[n++] = it->valuestring;
        }
    }
    return n;
}

/* ---- the OWNED ask snapshot (kills the 120 s use-after-free) ------------- *
 * Bounded copy into caller storage. No malloc, no float, no aliasing: after
 * this returns, `out` is independent of the parse tree and survives
 * jr_gemini_parse_free() — which the pump calls the instant the rich callback
 * returns, 120 seconds before the human finishes deciding. */
static bool copy_bounded(char *dst, size_t cap, const char *src)
{
    /* returns true if the whole source fitted */
    size_t i = 0;
    if (!src) { dst[0] = '\0'; return true; }
    while (src[i] != '\0' && i + 1u < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return src[i] == '\0';
}

bool jr_gemini_event_to_ask(const jr_gemini_event_t *ev, jr_gemini_ask_t *out)
{
    if (!out) { return false; }
    memset(out, 0, sizeof(*out));   /* inert on every failure path */
    if (!ev || ev->kind != JR_GEV_TOOL_CALL) { return false; }
    if (!ev->tool_name || strcmp(ev->tool_name, JR_GEMINI_ASK_USER_TOOL) != 0) {
        return false;
    }
    /* No text id means no answer can ever be addressed: the hash alone cannot
     * rebuild the functionResponse Gemini expects. Accepting such an ask would
     * wedge the Asking state for the full 120 s and then still fail to answer.
     * Reject it here so the caller's malformed-ask path handles it instead. */
    if (!ev->call_id_text || ev->call_id_text[0] == '\0') {
        return false;
    }

    const char *q = jr_gemini_tool_arg_string(ev, JR_GEMINI_ASK_USER_ARG_QUESTION);
    if (!q || q[0] == '\0') { return false; }

    /* Read ONE past the arc budget purely to detect an over-long list — a
     * dropped option is a real loss of user choice and must be reported, not
     * absorbed. */
    const char *opts[JR_GEMINI_ASK_USER_MAX_CHOICES + 1u];
    size_t n = jr_gemini_tool_arg_string_array(
        ev, JR_GEMINI_ASK_USER_ARG_OPTIONS, opts,
        JR_GEMINI_ASK_USER_MAX_CHOICES + 1u);

    bool whole = true;
    if (n > JR_GEMINI_ASK_USER_MAX_CHOICES) {
        n = JR_GEMINI_ASK_USER_MAX_CHOICES;
        whole = false;
    }
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (!opts[i] || opts[i][0] == '\0') { continue; }   /* an empty arc is not a choice */
        whole &= copy_bounded(out->options[kept], JR_GEMINI_ASK_OPTION_CAP, opts[i]);
        kept++;
    }
    if (kept == 0) { memset(out, 0, sizeof(*out)); return false; }

    whole &= copy_bounded(out->question, JR_GEMINI_ASK_QUESTION_CAP, q);
    whole &= copy_bounded(out->call_id, JR_GEMINI_ASK_CALL_ID_CAP, ev->call_id_text);
    out->call_id_hash = ev->call_id;
    out->count        = (uint8_t)kept;
    /* The model was told at most three options; a longer list lost arcs. */
    out->truncated    = !whole;
    return true;
}

/* ======================================================================== *
 *  CLIENT / FRAMER                                                          *
 * ======================================================================== */
static uint64_t now_ms(jr_gemini_client_t *c)
{
    return jr_clock_now_ms(&c->clk);
}

/* Feed one tx outcome to the liveness struct + the (optional) DeadUplinkMonitor.
 * would-block is backpressure (ignored); a soft-fail counts; a good send re-arms. */
static void feed_tx(jr_gemini_client_t *c, jr_err_t r)
{
    if (r == JR_OK) {
        c->live.consecutive_tx_failures = 0;
    } else if (r == JR_ERR_FAIL) {
        c->live.consecutive_tx_failures++;
    } else if (r == JR_ERR_WOULD_BLOCK) {
        c->live.tx_would_block++;
    }
    if (c->dead_uplink) {
        jr_dead_uplink_on_tx(c->dead_uplink, r);
    }
}

/* Enqueue a (possibly partial) frame; drop-newest when the bounded ring is full
 * or the frame exceeds a slot. Hard-bounded memory under a starved consumer. */
static void txq_enqueue(jr_gemini_client_t *c, const char *data, size_t len)
{
    if (len > JR_GEMINI_TXQ_SLOT || c->depth == JR_GEMINI_TXQ_DEPTH) {
        c->live.tx_drops++;   /* drop-newest */
        return;
    }
    jr_gemini_txframe_t *f = &c->txq[c->tail];
    memcpy(f->buf, data, len);
    f->len = len;
    f->off = 0;
    c->tail = (c->tail + 1) % JR_GEMINI_TXQ_DEPTH;
    c->depth++;
}

jr_err_t jr_gemini_flush(jr_gemini_client_t *c)
{
    while (c->depth > 0) {
        jr_gemini_txframe_t *f = &c->txq[c->head];
        jr_ws_send_result_t r = c->ws.send_bytes(c->ws.ctx,
                                                  (const uint8_t *)(f->buf + f->off),
                                                  f->len - f->off);
        switch (r.kind) {
        case JR_WS_TX_OK:
            c->head = (c->head + 1) % JR_GEMINI_TXQ_DEPTH;
            c->depth--;
            feed_tx(c, JR_OK);
            break;                       /* try the next buffered frame */
        case JR_WS_TX_PARTIAL:
            f->off += r.sent;
            feed_tx(c, JR_ERR_WOULD_BLOCK);
            return JR_ERR_WOULD_BLOCK;    /* socket filled again */
        case JR_WS_TX_WOULD_BLOCK:
            feed_tx(c, JR_ERR_WOULD_BLOCK);
            return JR_ERR_WOULD_BLOCK;
        case JR_WS_TX_SOFT_FAIL:
            feed_tx(c, JR_ERR_FAIL);      /* count; keep the connection */
            return JR_ERR_WOULD_BLOCK;
        case JR_WS_TX_CLOSED:
            c->live.socket_open = false;
            feed_tx(c, JR_ERR_FAIL);
            return JR_ERR_CLOSED;         /* the death path */
        }
    }
    return JR_OK;
}

void jr_gemini_reset_tx(jr_gemini_client_t *c)
{
    if (c == NULL) {
        return;
    }
    c->head = 0;
    c->tail = 0;
    c->depth = 0;
    c->live.consecutive_tx_failures = 0;
}

jr_err_t jr_gemini_send_frame(jr_gemini_client_t *c, const char *json, size_t len)
{
    /* Drain anything buffered first so frame ORDER is preserved. */
    jr_err_t fr = jr_gemini_flush(c);
    if (fr == JR_ERR_CLOSED) {
        return JR_ERR_CLOSED;
    }
    if (c->depth > 0) {
        /* still blocked — buffer behind the pending frames (drop-newest if full) */
        txq_enqueue(c, json, len);
        feed_tx(c, JR_ERR_WOULD_BLOCK);
        return JR_ERR_WOULD_BLOCK;
    }

    jr_ws_send_result_t r = c->ws.send_bytes(c->ws.ctx, (const uint8_t *)json, len);
    switch (r.kind) {
    case JR_WS_TX_OK:
        feed_tx(c, JR_OK);
        return JR_OK;
    case JR_WS_TX_PARTIAL:
        txq_enqueue(c, json + r.sent, len - r.sent);
        feed_tx(c, JR_ERR_WOULD_BLOCK);
        return JR_ERR_WOULD_BLOCK;
    case JR_WS_TX_WOULD_BLOCK:
        txq_enqueue(c, json, len);
        feed_tx(c, JR_ERR_WOULD_BLOCK);
        return JR_ERR_WOULD_BLOCK;
    case JR_WS_TX_SOFT_FAIL:
        /* socket still OPEN: count for the DeadUplinkMonitor, keep the
         * connection. A single soft-fail is NOT a teardown. */
        feed_tx(c, JR_ERR_FAIL);
        return JR_ERR_WOULD_BLOCK;
    case JR_WS_TX_CLOSED:
        c->live.socket_open = false;
        feed_tx(c, JR_ERR_FAIL);
        return JR_ERR_CLOSED;
    }
    return JR_ERR_FAIL;   /* unreachable */
}

static void emit_neutral(jr_gemini_client_t *c, const jr_gemini_event_t *ge)
{
    if (!c->rvc_cb) { return; }
    jr_rvc_event_t n;
    memset(&n, 0, sizeof(n));
    switch (ge->kind) {
    case JR_GEV_SETUP_COMPLETE:      n.kind = JR_RVC_EV_SETUP_COMPLETE; break;
    case JR_GEV_AUDIO_CHUNK:
        n.kind = JR_RVC_EV_AUDIO_CHUNK;
        n.audio = ge->pcm;
        n.audio_samples = ge->pcm_len;
        break;
    case JR_GEV_INTERRUPTED:         n.kind = JR_RVC_EV_INTERRUPTED; break;
    case JR_GEV_TURN_COMPLETE:
    case JR_GEV_GENERATION_COMPLETE: n.kind = JR_RVC_EV_TURN_COMPLETE; break;
    case JR_GEV_TOOL_CALL:
    case JR_GEV_TOOL_CANCEL:         n.kind = JR_RVC_EV_TOOL_CALL; break;
    case JR_GEV_RESUMPTION_UPDATE:
    case JR_GEV_GO_AWAY:             n.kind = JR_RVC_EV_HEARTBEAT; break;
    case JR_GEV_ERROR:
        n.kind = JR_RVC_EV_ERROR;
        n.error = JR_ERR_FAIL;   /* the rich cb carries the fine-grained kind */
        break;
    case JR_GEV_TEXT:
    case JR_GEV_UNKNOWN:
    default:
        return;                  /* no neutral surface for these */
    }
    c->rvc_cb(c->rvc_user, &n);
}

bool jr_gemini_pump_rx(jr_gemini_client_t *c)
{
#ifdef ESP_PLATFORM
    if (c->rx_buf == NULL) {
        c->rx_buf = (char *)malloc(JR_GEMINI_RX_MAX);
        if (c->rx_buf == NULL) {
            c->live.rx_alloc_failures++;
            return false;
        }
        c->rx_cap = JR_GEMINI_RX_MAX;
    }
    char *rx_buf = c->rx_buf;
    size_t rx_cap = c->rx_cap;
#else
    /* The host runner has a conventional multi-megabyte stack and benefits
     * from allocation-free leak checks. Device builds use persistent heap/PSRAM
     * scratch above so the FreeRTOS owner stack stays small. */
    char host_rx_buf[JR_GEMINI_RX_MAX];
    char *rx_buf = host_rx_buf;
    size_t rx_cap = sizeof host_rx_buf;
#endif
    int n = c->ws.recv_frame(c->ws.ctx, rx_buf, rx_cap);
    if (n <= 0) {
        return false;
    }
    rx_buf[n] = '\0';

    /* liveness: ANY inbound frame (incl. heartbeat) re-arms the KeepaliveMonitor
     * — v4 discarded heartbeats; here they are authoritative (spec §5.2). The
     * injected clock is the single time source (ws.last_rx_ms is the adapter's
     * own diagnostic mirror). */
    uint64_t now = now_ms(c);
    c->live.last_rx_ms = now;
    if (c->keepalive) {
        jr_keepalive_on_traffic(c->keepalive, now);
    }

    jr_gemini_parse_t pr;
    if (jr_gemini_parse(rx_buf, &pr)) {
        for (size_t i = 0; i < pr.count; i++) {
            if (c->rich_cb) { c->rich_cb(c->rich_user, &pr.events[i]); }
            emit_neutral(c, &pr.events[i]);
        }
        jr_gemini_parse_free(&pr);
    } else {
        c->live.rx_parse_errors++;
    }
    return true;
}

void jr_gemini_client_init(jr_gemini_client_t *c, jr_ws_transport_t ws,
                           jr_clock_t clk, const jr_gemini_config_t *cfg)
{
    memset(c, 0, sizeof(*c));
    c->ws = ws;
    c->clk = clk;
    if (cfg) { c->cfg = *cfg; }
    c->live.socket_open = false;
}

void jr_gemini_client_deinit(jr_gemini_client_t *c)
{
    if (c == NULL) {
        return;
    }
    free(c->rx_buf);
    c->rx_buf = NULL;
    c->rx_cap = 0;
}

void jr_gemini_client_set_monitors(jr_gemini_client_t *c,
                                   jr_keepalive_monitor_t *ka,
                                   jr_dead_uplink_monitor_t *du)
{
    c->keepalive = ka;
    c->dead_uplink = du;
}

void jr_gemini_client_set_event_cb(jr_gemini_client_t *c,
                                   jr_gemini_event_cb cb, void *user)
{
    c->rich_cb = cb;
    c->rich_user = user;
}

/* ---- provider-neutral RealtimeVoiceClient view (upper seam) ---- */
static void rvc_set_event_cb(void *ctx, jr_rvc_event_cb cb, void *user)
{
    jr_gemini_client_t *c = (jr_gemini_client_t *)ctx;
    c->rvc_cb = cb;
    c->rvc_user = user;
}

static jr_err_t rvc_connect(void *ctx)
{
    jr_gemini_client_t *c = (jr_gemini_client_t *)ctx;
    jr_err_t e = c->ws.connect(c->ws.ctx, c->cfg.url ? c->cfg.url : "");
    if (e != JR_OK) { return e; }
    c->live.socket_open = true;
    /* Setup is a Gemini protocol detail hidden behind the neutral port: the
     * neutral consumer only sees SETUP_COMPLETE later. */
    char *setup = jr_gemini_build_setup(&c->cfg);
    if (!setup) { return JR_ERR_NO_MEM; }
    jr_err_t r = jr_gemini_send_frame(c, setup, strlen(setup));
    free(setup);
    /* A would-block on setup is backpressure (buffered) — still success. */
    return (r == JR_ERR_CLOSED) ? JR_ERR_CLOSED : JR_OK;
}

static jr_err_t rvc_send_audio(void *ctx, const jr_pcm_t *frame, size_t samples)
{
    jr_gemini_client_t *c = (jr_gemini_client_t *)ctx;
    char *json = jr_gemini_build_audio_chunk(frame, samples, JR_GEMINI_TX_RATE);
    if (!json) { return JR_ERR_NO_MEM; }
    jr_err_t r = jr_gemini_send_frame(c, json, strlen(json));
    free(json);
    return r;
}

static jr_err_t rvc_send_text(void *ctx, const char *text)
{
    jr_gemini_client_t *c = (jr_gemini_client_t *)ctx;
    char *json = jr_gemini_build_text_turn(text);
    if (!json) { return JR_ERR_NO_MEM; }
    jr_err_t r = jr_gemini_send_frame(c, json, strlen(json));
    free(json);
    return r;
}

static jr_err_t rvc_send_control(void *ctx, jr_rvc_control_t ctl)
{
    jr_gemini_client_t *c = (jr_gemini_client_t *)ctx;
    char *json = NULL;
    bool manual = (c->cfg.vad_mode != JR_VAD_SERVER);
    switch (ctl) {
    case JR_RVC_CTRL_ACTIVITY_START:
        /* Manual-mode turn boundary. In auto-VAD the server owns activity, so a
         * client activityStart is a no-op (matches firmware). */
        if (!manual) { return JR_OK; }
        json = jr_gemini_build_activity_start();
        break;
    case JR_RVC_CTRL_ACTIVITY_END:
        if (!manual) { return JR_OK; }
        json = jr_gemini_build_activity_end();
        break;
    case JR_RVC_CTRL_AUDIO_STREAM_END:
        /* CRITICAL GUARANTEE: audioStreamEnd is valid ONLY under auto server VAD.
         * Emitting it in manual/PTT mode was v4's stuck-in-THINKING bug — refuse
         * it here so it can NEVER be sent in manual mode. */
        if (manual) { return JR_OK; }
        json = jr_gemini_build_audio_stream_end();
        break;
    default:
        return JR_ERR_INVALID;
    }
    if (!json) { return JR_ERR_NO_MEM; }
    jr_err_t r = jr_gemini_send_frame(c, json, strlen(json));
    free(json);
    return r;
}

static void rvc_close(void *ctx)
{
    jr_gemini_client_t *c = (jr_gemini_client_t *)ctx;
    c->ws.close(c->ws.ctx);
    c->live.socket_open = false;
    jr_gemini_reset_tx(c);
}

jr_realtime_voice_client_t jr_gemini_client_as_rvc(jr_gemini_client_t *c)
{
    jr_realtime_voice_client_t v;
    v.ctx = c;
    v.set_event_cb = rvc_set_event_cb;
    v.connect = rvc_connect;
    v.send_audio = rvc_send_audio;
    v.send_text = rvc_send_text;
    v.send_control = rvc_send_control;
    v.close = rvc_close;
    return v;
}
