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
    /* A board template: the program starts with `const PJ=<project id>;`
     * so the same bytes address whichever board the device is paired to. */
    bool board;
} template_desc_t;

static char s_board_project[JR_TOOLS_BOARD_PROJECT_CAP] =
    JR_TOOLS_BOARD_PROJECT_DEFAULT;

bool jr_tools_set_board_project(const char *project_id)
{
    if (project_id == NULL || project_id[0] == '\0') {
        strcpy(s_board_project, JR_TOOLS_BOARD_PROJECT_DEFAULT);
        return true;
    }
    const size_t len = strnlen(project_id, JR_TOOLS_BOARD_PROJECT_CAP);
    if (len >= JR_TOOLS_BOARD_PROJECT_CAP) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const unsigned char ch = (unsigned char)project_id[i];
        if (!isalnum(ch) && ch != '.' && ch != '_' && ch != '-') {
            return false;
        }
    }
    memcpy(s_board_project, project_id, len + 1U);
    return true;
}

const char *jr_tools_board_project(void)
{
    return s_board_project;
}

/* These strings are the complete executable vocabulary. There is no generic
 * `code` tool and no path that copies model text outside a quoted literal. */
static const template_desc_t s_templates[] = {
    /* PROJECTED: five raw hits ran to 3.6 KB against the 3 KB slot and were
     * cut mid-JSON, so the model read a broken answer and said it remembered
     * nothing. Title, date, and 200 characters of snippet is what a voice
     * answer needs. */
    {"recall_memory", "query", 255U, false,
     "const r=await jarvis.memory.search({query:", ",area:\"all\",limit:5});"
     "return {hits:(r.results||[]).slice(0,5).map(h=>({title:h.title||h.path,"
     "when:h.updated||h.date||\"\",text:String(h.snippet||h.body||\"\")"
     ".replace(/^---[\\s\\S]*?---\\s*/,\"\").slice(0,200)})),"
     "total:r.total_scanned||0}", false},
    /* 200 characters now: the note is no longer shown for a tap, so the
     * panel's 47-glyph limit no longer applies; the character whitelist
     * stays, it is what keeps the note a string and not a program. The
     * title is the note's own first words, readable in the vault. */
    {"remember", "note", 200U, false,
     "const b=", ";"
     "return await jarvis.memory.capture({type:\"note\","
     "title:b.slice(0,60),body:b,"
     "tags:[\"jarvisnano\",\"voice\"],source:\"jarvisnano\","
     "confidence:\"confirmed\"})", false},
    {"current_time", "timezone", 79U, true,
     "return await jarvis.time(", ")", false},
    /* The WEATHER screen's own fetch: no model, no arguments, the city in
     * code (Fort Lauderdale) per the repo rule that only endpoints and keys
     * live in NVS. Projected to what the glass shows, nothing more. Metric
     * in, converted on the device. */
    {"weather_glance", "query", 1U, true,
     "const w=await jarvis.weather(26.1224,-80.1373);const c=w.current||{};"
     "const d=(w.daily||[])[0]||{};return {t:c.temperature,f:c.feelsLike,"
     "h:c.humidity,c:String(c.condition||\"\"),ws:c.windSpeed,hi:d.tempMax,"
     "lo:d.tempMin,r:d.precipitation,dc:String(d.condition||\"\")}", "", false},
    /* PROJECTED, not raw: three raw matches were 4-5 KB against a 3 KB
     * device budget and were cut mid-JSON, so Gemini read a broken answer.
     * Eight matches projected to {tool, what, params} are ~1.6 KB, and the
     * three basics are pinned so a web question never depends on ranking. */
    {"search_tools", "query", 191U, false,
     "const r=await jarvis.meta.search(",
     ",{limit:8,detail:\"compact\"});"
     "const seen=new Set();const out=[];"
     "for(const m of (r.matches||[])){const t=m.method&&m.method.startsWith(m.service+\".\")?m.method:(m.service===m.method?m.method:m.service+\".\"+m.method);"
     "if(seen.has(t))continue;seen.add(t);"
     "out.push({tool:t,what:String(m.description||\"\").replace(/\\[[^\\]]*\\]\\s*/,\"\").slice(0,140),"
     "params:Object.keys(m.parameters||{}).join(\",\")})}"
     "for(const b of [[\"websearch\",\"Search the live web; returns titles, links, snippets\",\"query\"],"
     "[\"weather\",\"Current weather + forecast for coordinates\",\"latitude,longitude\"],"
     "[\"wiki\",\"Wikipedia summary\",\"query\"]]){if(!seen.has(b[0]))out.push({tool:b[0],what:b[1],params:b[2]})}"
     "return {tools:out,has_more:!!(r.page&&r.page.hasMore),note:\"call execute_tool with tool and args_json keyed by params, in that order\"}", false},
    /* HANDS ELSEWHERE. A spoken job becomes one work item on the durable
     * board (PJ is the paired project, spliced in by jr_tools_build_code);
     * a worker on another machine claims it, and the device announces the
     * result from board_poll. The tool returns in one round trip and never
     * waits: the 30 s sandbox and the utterance watchdog both forbid it.
     * Identity is the device's own, the same as execute_tool's board path.
     * Priority is not an argument: the template contract is one string, and
     * the worker treats every spoken job as normal. */
    {"delegate_task", "goal", 600U, false,
     "const g=",
     ";const r=await jarvis.coordination.createWorkItem({projectId:PJ,"
     "title:g.slice(0,80),description:g,priority:\"normal\","
     "identity:{runtime:\"pi\",agentId:\"jarvisnano\",hostId:\"jarvisnano\","
     "sessionId:new Date().toISOString().slice(0,10)}});"
     "return {id:String(r.id||r.workItemId||\"\").slice(0,40),"
     "title:String(r.title||g.slice(0,80)),status:String(r.status||\"queued\")}",
     true},
    /* The board as a voice answer: six items, newest first, 60 glyphs of
     * title and 120 of result. Nothing raw crosses the 3 KB response. */
    {"delegated_tasks", "query", 1U, true,
     "const L=await jarvis.coordination.listWorkItems({projectId:PJ,detail:\"summary\"});"
     "const a=(Array.isArray(L)?L:(L&&(L.items||L.workItems))||[]).slice();"
     "const k=w=>String(w.updatedAt||w.completedAt||w.lastProgressAt||w.createdAt||\"\");"
     "a.sort((x,y)=>k(y).localeCompare(k(x)));"
     "return {tasks:a.slice(0,6).map(w=>({id:String(w.id||\"\").slice(0,12),"
     "title:String(w.title||\"\").slice(0,60),status:String(w.status||\"\"),"
     "result:String(w.resultSummary||w.blockReason||w.blockedReason||w.reason||w.decisionNeeded||\"\").slice(0,120)})),"
     "total:a.length}", "", true},
    /* The device's own poll (never declared to the model), and the worker.
     * Every 90 s the device is the scheduler's heartbeat and the gateway does
     * the work in the same call: one eligible item is claimed (or an expired
     * lease recovered), then either handed to the managed Pi sandbox worker
     * (a goal that names owner/repo and says repo/branch/PR — delivery is a
     * branch) or answered by the gateway's cited research agent with the
     * owner's own notes as context, filed into the company brain, and
     * completed with a <=300-char summary; any failure blocks the item with
     * the reason. Then the six most recently touched items come back as
     * {i,s,n,r} — id, status, name, result — so the device can announce a
     * finish once. Measured 21 s for a research item; the 30 s gateway cap
     * is why it is one item per poll. Nothing runs on any other host. */
    {"board_poll", "query", 1U, true,
     "const ID={runtime:\"pi\",agentId:\"jarvisnano-desk-worker\",hostId:\"jarvismcp\",sessionId:new Date().toISOString().slice(0,10)};"
     "const C=jarvis.coordination;const B={projectId:PJ,identity:ID};"
     "const ls=async()=>{const L=await C.listWorkItems({projectId:PJ,detail:\"summary\"});"
     "const a=(Array.isArray(L)?L:(L&&(L.items||L.workItems))||[]).slice();"
     "const k=w=>String(w.updatedAt||w.completedAt||w.lastProgressAt||w.createdAt||\"\");"
     "a.sort((x,y)=>k(y).localeCompare(k(x)));return a};"
     "let a=await ls();const st=w=>String(w.status||\"\").toLowerCase();"
     "const act=w=>String(w.leaseState||\"\")===\"active\";"
     "const c=a.find(w=>st(w)===\"open\"&&!act(w))||a.find(w=>st(w)===\"in_progress\"&&String(w.leaseState||\"\")===\"expired\");"
     "let did=\"\";if(c){const id=c.id,goal=String(c.title||\"\");try{"
     "if(st(c)===\"open\")await C.claimWorkItem({...B,workItemId:id,leaseSeconds:180});"
     "else await C.recoverWorkItem({...B,workItemId:id,recoveryId:\"rec-\"+id,idempotencyKey:\"rec-\"+id+\"-\"+Date.now().toString(36),reason:\"lease expired mid-run\",evidence:[\"board_poll reconciler\"],leaseSeconds:180});"
     "const m=goal.match(/\\b([A-Za-z0-9_.-]+\\/[A-Za-z0-9_.-]+)\\b/);"
     "if(m&&/\\b(repo|github|branch|pull request|PR)\\b/i.test(goal)){"
     "const j=await jarvis.sandboxes.workerSubmit({idempotencyKey:\"jn-\"+id,kind:\"pi\",delivery:\"branch\",repo:m[1],task:goal,maxRuntimeSeconds:1800});"
     "const s=String(j.status||\"\");"
     "if(s===\"succeeded\")await C.completeWorkItem({...B,workItemId:id,result:{summary:\"Delivered on \"+m[1]+(j.delivery&&j.delivery.branch?\", branch \"+j.delivery.branch:\"\")+\".\",evidence:[\"worker \"+j.jobId]}});"
     "else if(s===\"failed\"||s===\"canceled\")await C.blockWorkItem({...B,workItemId:id,reason:\"worker \"+s,evidence:[\"worker \"+j.jobId]});"
     "else await C.reportProgress({...B,workItemId:id,message:\"worker \"+s,evidence:[\"worker \"+j.jobId]});did=s;"
     "}else{"
     "let ctx=\"\";try{const h=await jarvis.memory.search(goal,{limit:3});const hs=Array.isArray(h)?h:(h&&h.results)||[];"
     "ctx=hs.map(x=>String(x.snippet||x.excerpt||x.content||\"\").replace(/\\s+/g,\" \").slice(0,300)).filter(Boolean).join(\" | \")}catch(e){}"
     "const r=await jarvis.research((ctx?\"Context from the owner's own notes: \"+ctx+\"\\n\\nTask: \":\"\")+goal,{queries:2,maxSources:5});"
     "const ans=String(r&&r.answer||\"\").replace(/\\[\\d+\\]/g,\"\").replace(/[#*_`>|]+/g,\"\").replace(/\\s+/g,\" \").trim();"
     "if(!ans)throw new Error(\"research returned nothing\");const src=(r.sources||[]).slice(0,6);"
     "try{await jarvis.memory.capture({type:\"note\",source:\"jarvisnano-desk\",title:goal.slice(0,60),body:String(r.answer)+\"\\n\\nSources:\\n\"+src.map(s=>\"[\"+s.n+\"] \"+s.title+\" \"+s.url).join(\"\\n\")})}catch(e){}"
     "await C.completeWorkItem({...B,workItemId:id,result:{summary:ans.slice(0,300),evidence:src.map(s=>String(s.url)).slice(0,5)}});did=\"done\";"
     "}}catch(e){try{await C.blockWorkItem({...B,workItemId:id,reason:String(e&&e.message||e).slice(0,160)})}catch(e2){}did=\"blocked\"}"
     "a=await ls();}"
     "return {d:did,t:a.slice(0,6).map(w=>({i:String(w.id||\"\").slice(0,40),"
     "s:String(w.status||\"\").slice(0,12),n:String(w.title||\"\").slice(0,48),"
     "r:String(w.resultSummary||w.blockReason||w.decisionNeeded||\"\").slice(0,120)}))}",
     "", true},
};

/* The execute_tool program. %s twice: the path array, then the args object.
 * ORD pins the documented positional order for the tools a voice assistant
 * actually reaches for, because Object.values() follows the model's key
 * order and weather(longitude, latitude) is a different place. */
#define JR_TOOLS_EXEC_JS \
    "const P=%s;let A=%s;const ORD={websearch:[\"query\"],weather:[\"latitude\",\"longitude\"],wiki:[\"query\"],time:[\"timezone\"],crypto:[\"coin\",\"currency\"],exchange:[\"base\",\"targets\"],country:[\"name\"],geocode:[\"query\"],translate:[\"text\",\"target\",\"source\"],hackernews:[\"type\",\"count\"],research:[\"question\"],\"stocks.quote\":[\"symbols\"],\"reddit.search\":[\"query\"],\"memory.search\":[\"query\"],\"holidays.list\":[\"country\",\"year\"]};const name=P.join(\".\");let o=jarvis;for(const k of P.slice(0,-1)){o=o[k];if(o==null)return{error:\"unknown tool\"}}const f=o[P[P.length-1]];if(typeof f!==\"function\")return{error:\"unknown tool\"};const board=P[0]===\"coordination\";const note=name===\"memory.capture\";if(board){const i=A.input||A;if(!i.identity)i.identity={runtime:\"pi\",agentId:\"jarvisnano\",hostId:\"jarvisnano\",sessionId:new Date().toISOString().slice(0,10)};A=i}if(note){const i=A.input||A;i.type=i.type||\"note\";i.source=i.source||\"jarvisnano\";i.title=i.title||String(i.body||\"\").slice(0,60);A=i}const named=P.length>=3||board||note;const ord=ORD[name];const v=ord?ord.map(k=>A[k]):Object.values(A);const first=named?[A]:v;const second=named?v:[A];let r;try{r=await f.call(o,...first)}catch(e1){try{r=await f.call(o,...second)}catch(e2){return{error:String(e1&&e1.message||e1).slice(0,200)}}}if(board){const L=Array.isArray(r)?r:(r&&(r.workItems||r.items));if(Array.isArray(L))r={total:L.length,items:L.slice(0,6).map(w=>({id:String(w.id||\"\").slice(0,12),title:String(w.title||\"\").slice(0,60),status:w.status,result:String(w.resultSummary||\"\").slice(0,120)}))}}const T=(x,d)=>{if(typeof x===\"string\")return x.length>320?x.slice(0,320)+\"\\u2026\":x;if(Array.isArray(x))return x.slice(0,6).map(y=>T(y,d+1));if(x&&typeof x===\"object\"&&d<4){const q={};for(const[k,y]of Object.entries(x).slice(0,24))q[k]=T(y,d+1);return q}return x};return T(r,0)"

/* `const PJ="<project>";` for board templates, "" for the rest. The id
 * passed jr_tools_set_board_project's charset, so it needs no escaping. */
static const char *board_prelude(const template_desc_t *desc)
{
    static char prelude[16U + JR_TOOLS_BOARD_PROJECT_CAP];
    if (!desc->board) {
        return "";
    }
    snprintf(prelude, sizeof prelude, "const PJ=\"%s\";", s_board_project);
    return prelude;
}

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
        if (!valid) {
            cJSON_Delete(nested);
            cJSON_Delete(args);
            return JR_TOOL_TEMPLATE_INVALID_ARGS;
        }
        /* THE DEVICE'S OWN POLICY. The legacy bearer carries the gateway
         * authority of its key — a probe from this session ran
         * dokploy.project.all() with it — and the typed route that would
         * enforce policy server-side is not provisioned. So the device only
         * ever generates calls into read-only services. Anything else is
         * refused here, before any code exists. */
        static const char *const allow[] = {
            "websearch", "research", "fetch", "wiki", "weather", "geocode",
            "country", "crypto", "exchange", "stocks", "massive", "time",
            "holidays", "hackernews", "reddit", "rss", "gdelt", "polymarket",
            "arxiv", "pubmed", "scholar", "openalex", "edgar", "openfda",
            "translate", "dns", "whois", "ipgeo", "cve", "pkg", "vuln",
            "npmsearch", "dockerhub", "context7", "memory.search",
            "memory.list", "memory.read", "meta.search", "meta.help",
            "assets.search",
            /* The owner's life and work, 2026-09-01: notes into the company
             * brain (append-only events), the work board, the CRM calendar
             * and Overwatch. The servers gate their own dangerous methods
             * (proposals, confirm flags); the device refuses the plainly
             * destructive names below regardless. */
            "memory.capture", "coordination", "butlercrm", "overwatch",
        };
        static const char *const deny_prefix[] = {
            "delete", "remove", "destroy", "purge", "wipe", "archive",
        };
        bool allowed = false;
        for (size_t i = 0; i < sizeof allow / sizeof allow[0]; ++i) {
            const size_t n = strlen(allow[i]);
            if (strncmp(tool->valuestring, allow[i], n) == 0 &&
                (tool->valuestring[n] == '\0' || tool->valuestring[n] == '.')) {
                allowed = true;
                break;
            }
        }
        if (allowed) {
            const char *last = strrchr(tool->valuestring, '.');
            last = last != NULL ? last + 1 : tool->valuestring;
            for (size_t i = 0; i < sizeof deny_prefix / sizeof deny_prefix[0]; ++i) {
                if (strncasecmp(last, deny_prefix[i], strlen(deny_prefix[i])) == 0) {
                    allowed = false;
                    break;
                }
            }
        }
        if (!allowed) {
            cJSON_Delete(nested);
            cJSON_Delete(args);
            int written = snprintf(out, out_cap, "%s",
                "return {error:\"that tool is not permitted from the device\"}");
            return (written > 0 && (size_t)written < out_cap)
                       ? JR_TOOL_TEMPLATE_OK : JR_TOOL_TEMPLATE_TOO_LARGE;
        }
        /* Path as a JSON array of segments, resolved with bracket access on
         * the gateway, so a name can never read as an expression. Args are
         * re-printed by cJSON: a JSON literal, never code. Positional first
         * (most jarvis tools are), named on failure. */
        cJSON *path = cJSON_CreateArray();
        char seg[JR_TOOLS_NAME_CAP];
        size_t si = 0;
        for (size_t i = 0; path != NULL && i <= tool_len; ++i) {
            const char ch = tool->valuestring[i];
            if (ch == '.' || ch == '\0') {
                seg[si] = '\0';
                cJSON_AddItemToArray(path, cJSON_CreateString(seg));
                si = 0;
            } else if (si + 1U < sizeof seg) {
                seg[si++] = ch;
            }
        }
        char *path_json = path ? cJSON_PrintUnformatted(path) : NULL;
        char *args_out = cJSON_PrintUnformatted(nested);
        cJSON_Delete(path);
        cJSON_Delete(nested);
        cJSON_Delete(args);
        if (path_json == NULL || args_out == NULL) {
            free(path_json);
            free(args_out);
            return JR_TOOL_TEMPLATE_TOO_LARGE;
        }
        int written = snprintf(out, out_cap, JR_TOOLS_EXEC_JS, path_json, args_out);
        free(path_json);
        free(args_out);
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
            int n = snprintf(out, out_cap, "%s%s%s", board_prelude(desc),
                             desc->prefix, desc->suffix);
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
        int n = snprintf(out, out_cap, "%s%s%s%s", board_prelude(desc),
                         desc->prefix, literal, desc->suffix);
        if (n < 0 || (size_t)n >= out_cap) {
            out[0] = '\0';
            status = JR_TOOL_TEMPLATE_TOO_LARGE;
        }
    }
    free(literal);
    cJSON_Delete(args);
    return status;
}
