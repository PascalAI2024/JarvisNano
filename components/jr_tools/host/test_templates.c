/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The template vocabulary, pinned. Every string here is what the device
 * will hand the gateway for that tool and those arguments; a change to a
 * template that is not also a change here is a silent change to what the
 * device can do. Rewritten 2026-09-02: the previous file pinned a vocabulary
 * (crypto_price, wikipedia, country_info) that had left the firmware months
 * earlier, so the suite had been red and nobody ran it.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jr_tools/jr_tools.h"

static int g_checks;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            printf("FAIL line %d: %s\n", __LINE__, what);                     \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static char g_out[4096];   /* JR_TOOLS_CODE_CAP: board_poll alone is ~3 KB */

static jr_tool_template_status_t build(const char *name, const char *args)
{
    memset(g_out, 0, sizeof g_out);
    return jr_tools_build_code(name, args, g_out, sizeof g_out);
}

static int starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int test_fixed_templates(void)
{
    CHECK(build("recall_memory", "{\"query\":\"release plan\"}") == JR_TOOL_TEMPLATE_OK,
          "recall_memory builds");
    CHECK(starts(g_out, "const r=await jarvis.memory.search({query:\"release plan\",area:\"all\",limit:5});"),
          "recall_memory searches all areas, five hits");
    CHECK(strstr(g_out, ".slice(0,200)") != NULL, "recall_memory projects 200 chars of snippet");

    CHECK(build("remember", "{\"note\":\"Pascal's tea, (green & hot)\"}") == JR_TOOL_TEMPLATE_OK,
          "remember builds");
    CHECK(strcmp(g_out,
                 "const b=\"Pascal's tea, (green & hot)\";"
                 "return await jarvis.memory.capture({type:\"note\","
                 "title:b.slice(0,60),body:b,"
                 "tags:[\"jarvisnano\",\"voice\"],source:\"jarvisnano\","
                 "confidence:\"confirmed\"})") == 0,
          "remember is one capture event");

    CHECK(build("current_time", "{}") == JR_TOOL_TEMPLATE_OK, "current_time builds");
    CHECK(strcmp(g_out, "return await jarvis.time(\"America/New_York\")") == 0,
          "current_time defaults to the owner's zone");
    CHECK(build("current_time", "{\"timezone\":\"Europe/London\"}") == JR_TOOL_TEMPLATE_OK,
          "current_time with a zone builds");
    CHECK(strcmp(g_out, "return await jarvis.time(\"Europe/London\")") == 0,
          "current_time passes the zone");

    CHECK(build("weather_glance", "{}") == JR_TOOL_TEMPLATE_OK, "weather_glance builds");
    CHECK(starts(g_out, "const w=await jarvis.weather(26.1224,-80.1373);"),
          "weather_glance is Fort Lauderdale in code");

    CHECK(build("search_tools", "{\"query\":\"read calendar events\"}") == JR_TOOL_TEMPLATE_OK,
          "search_tools builds");
    CHECK(starts(g_out, "const r=await jarvis.meta.search(\"read calendar events\",{limit:8,detail:\"compact\"});"),
          "search_tools asks for eight compact matches");
    return 0;
}

static int test_execute_tool_policy(void)
{
    CHECK(build("execute_tool",
                "{\"tool\":\"websearch\",\"args_json\":\"{\\\"query\\\":\\\"esp32\\\"}\"}") ==
              JR_TOOL_TEMPLATE_OK,
          "execute_tool websearch builds");
    CHECK(starts(g_out, "const P=[\"websearch\"];let A={\"query\":\"esp32\"};"),
          "path is an array, args a re-printed literal");

    CHECK(build("execute_tool", "{\"tool\":\"coordination.createWorkItem\",\"args_json\":\"{}\"}") ==
              JR_TOOL_TEMPLATE_OK,
          "the board is reachable");
    CHECK(build("execute_tool", "{\"tool\":\"coordination.deleteWorkItem\",\"args_json\":\"{}\"}") ==
              JR_TOOL_TEMPLATE_OK,
          "a denied name still builds a program");
    CHECK(strcmp(g_out, "return {error:\"that tool is not permitted from the device\"}") == 0,
          "and that program is the refusal");
    CHECK(build("execute_tool", "{\"tool\":\"dokploy.project.all\",\"args_json\":\"{}\"}") ==
              JR_TOOL_TEMPLATE_OK,
          "an unlisted service builds");
    CHECK(strcmp(g_out, "return {error:\"that tool is not permitted from the device\"}") == 0,
          "and is refused");

    CHECK(build("execute_tool",
                "{\"tool\":\"calendar.list_events;globalThis.pwned=true\",\"args_json\":\"{}\"}") ==
              JR_TOOL_TEMPLATE_INVALID_ARGS,
          "a name with code in it is invalid");
    CHECK(build("execute_tool", "{\"tool\":\"websearch\",\"args_json\":\"[]\"}") ==
              JR_TOOL_TEMPLATE_INVALID_ARGS,
          "args must be an object");
    CHECK(build("execute_tool",
                "{\"tool\":\"websearch\",\"args_json\":\"{}\",\"code\":\"return 1\"}") ==
              JR_TOOL_TEMPLATE_INVALID_ARGS,
          "a third field is invalid");
    return 0;
}

static int test_invalid_arguments(void)
{
    CHECK(build("remember", "{\"note\":\"x\\\");globalThis.pwned=true;//\"}") ==
              JR_TOOL_TEMPLATE_INVALID_ARGS,
          "a note with code in it is invalid");
    CHECK(build("remember", "{\"note\":\"save caf\xc3\xa9\"}") == JR_TOOL_TEMPLATE_INVALID_ARGS,
          "a note outside the panel charset is invalid");
    CHECK(build("ask_jarvis", "{\"code\":\"return 1\"}") == JR_TOOL_TEMPLATE_UNKNOWN_TOOL,
          "there is no code tool");
    CHECK(build("remember", "[]") == JR_TOOL_TEMPLATE_INVALID_ARGS, "args must be an object");
    CHECK(build("remember", "{}") == JR_TOOL_TEMPLATE_INVALID_ARGS, "a required arg is required");
    CHECK(build("recall_memory", "{broken") == JR_TOOL_TEMPLATE_INVALID_ARGS, "broken JSON");
    CHECK(build("recall_memory", "{\"query\":\"Ada\"} trailing") == JR_TOOL_TEMPLATE_INVALID_ARGS,
          "trailing bytes");
    CHECK(build("recall_memory", "{\"query\":\"Ada\",\"code\":\"return 1\"}") ==
              JR_TOOL_TEMPLATE_INVALID_ARGS,
          "an extra field");
    CHECK(build("remember", "{\"note\":\"approved\",\"note\":\"swapped\"}") ==
              JR_TOOL_TEMPLATE_INVALID_ARGS,
          "a duplicate field");

    char tiny[8];
    CHECK(jr_tools_build_code("current_time", "{}", tiny, sizeof tiny) ==
              JR_TOOL_TEMPLATE_TOO_LARGE,
          "a small buffer is reported, not overrun");
    return 0;
}

/* ---- hands elsewhere ---------------------------------------------------- */

static int test_delegate_task_is_one_work_item(void)
{
    jr_tools_set_board_project(NULL);
    CHECK(strcmp(jr_tools_board_project(), "jarvisnano-desk") == 0, "default project");

    CHECK(build("delegate_task", "{\"goal\":\"Find three CO5300 datasheets and note the QSPI ceiling\"}") ==
              JR_TOOL_TEMPLATE_OK,
          "delegate_task builds");
    CHECK(starts(g_out,
                 "const PJ=\"jarvisnano-desk\";"
                 "const g=\"Find three CO5300 datasheets and note the QSPI ceiling\";"
                 "const r=await jarvis.coordination.createWorkItem({projectId:PJ,"
                 "title:g.slice(0,80),description:g,priority:\"normal\","
                 "identity:{runtime:\"pi\",agentId:\"jarvisnano\",hostId:\"jarvisnano\","),
          "the goal becomes one work item on the paired project with the device's identity");
    CHECK(strstr(g_out, "return {id:String(r.id||r.workItemId||\"\").slice(0,40),"
                        "title:String(r.title||g.slice(0,80)),status:String(r.status||\"queued\")}") != NULL,
          "and returns id, title, status only");
    CHECK(strstr(g_out, "await") == strstr(g_out, "await jarvis.coordination.createWorkItem"),
          "one round trip: the only await is the create");
    CHECK(strlen(g_out) < 700U, "the program fits the code slot with room");

    char goal[700];
    memset(goal, 'g', sizeof goal - 1U);
    goal[sizeof goal - 1U] = '\0';
    char args[760];
    snprintf(args, sizeof args, "{\"goal\":\"%.601s\"}", goal);
    CHECK(build("delegate_task", args) == JR_TOOL_TEMPLATE_INVALID_ARGS,
          "601 characters is over the 600 cap");
    snprintf(args, sizeof args, "{\"goal\":\"%.600s\"}", goal);
    CHECK(build("delegate_task", args) == JR_TOOL_TEMPLATE_OK, "600 is the cap");
    CHECK(build("delegate_task", "{}") == JR_TOOL_TEMPLATE_INVALID_ARGS, "a goal is required");
    CHECK(build("delegate_task", "{\"goal\":\"x\",\"priority\":\"high\"}") ==
              JR_TOOL_TEMPLATE_INVALID_ARGS,
          "priority is not an argument");
    CHECK(build("delegate_task", "{\"goal\":\"a\\\");globalThis.pwned=1;//\"}") ==
              JR_TOOL_TEMPLATE_OK,
          "a hostile goal builds");
    CHECK(strstr(g_out, "const g=\"a\\\");globalThis.pwned=1;//\";") != NULL,
          "and stays inside its string literal");
    return 0;
}

static int test_delegated_tasks_and_poll_are_projected(void)
{
    jr_tools_set_board_project(NULL);
    CHECK(build("delegated_tasks", "{}") == JR_TOOL_TEMPLATE_OK, "delegated_tasks builds");
    CHECK(starts(g_out, "const PJ=\"jarvisnano-desk\";"
                        "const L=await jarvis.coordination.listWorkItems({projectId:PJ,detail:\"summary\"});"),
          "it lists the paired project in summary");
    CHECK(strstr(g_out, "a.slice(0,6)") != NULL, "six items");
    CHECK(strstr(g_out, "title:String(w.title||\"\").slice(0,60)") != NULL, "60 of title");
    CHECK(strstr(g_out, ".slice(0,120)})),total:a.length}") != NULL, "120 of result, and a total");
    CHECK(strstr(g_out, "id:String(w.id||\"\").slice(0,12)") != NULL, "a short id");
    CHECK(build("delegated_tasks", "{\"query\":\"\"}") == JR_TOOL_TEMPLATE_OK, "an empty query is fine");
    CHECK(build("delegated_tasks", "{\"status\":\"done\"}") == JR_TOOL_TEMPLATE_INVALID_ARGS,
          "no filter argument exists");

    CHECK(build("board_poll", "{}") == JR_TOOL_TEMPLATE_OK, "board_poll builds");
    CHECK(strstr(g_out, "return {d:did,t:a.slice(0,6).map(w=>({i:String(w.id||\"\").slice(0,40),"
                        "s:String(w.status||\"\").slice(0,12),n:String(w.title||\"\").slice(0,48),") != NULL,
          "the poll is {i,s,n,r} with a 40-glyph id the ring can dedupe on");
    /* The poll is also the worker: one item claimed, worked, settled, in
     * the gateway, in the same call. Pin the four verbs so a "simplification"
     * that drops the work cannot stay green. */
    CHECK(strstr(g_out, "C.claimWorkItem({...B,workItemId:id,leaseSeconds:180})") != NULL,
          "the poll claims one eligible item");
    CHECK(strstr(g_out, "jarvis.sandboxes.workerSubmit({idempotencyKey:\"jn-\"+id,kind:\"pi\",delivery:\"branch\"") != NULL,
          "a repo goal goes to the managed sandbox worker as a branch");
    CHECK(strstr(g_out, "jarvis.research((ctx?") != NULL &&
          strstr(g_out, "jarvis.memory.search(goal,{limit:3})") != NULL,
          "anything else is researched with the owner's notes as context");
    CHECK(strstr(g_out, "C.completeWorkItem({...B,workItemId:id,result:{summary:ans.slice(0,300)") != NULL &&
          strstr(g_out, "C.blockWorkItem({...B,workItemId:id,reason:String(e&&e.message||e).slice(0,160)})") != NULL,
          "success completes with a 300-char summary, any error blocks with the reason");
    CHECK(strlen(g_out) < 4096U - 256U, "the poll program leaves 256 B of headroom under the 4 KB code cap");
    /* Worst case on the wire: six of {i:40,s:12,n:48,r:120} + keys and
     * quotes = 6 * (220 + 34) < 1.6 KB, under the 3 KB response slot. */
    CHECK(6U * (40U + 12U + 48U + 120U + 34U) < 1600U, "worst-case poll under 1.6 KB");
    return 0;
}

static int test_board_project_is_a_literal_not_a_program(void)
{
    CHECK(jr_tools_set_board_project("igd.ops-2026_q3"), "letters, digits, . _ - accepted");
    CHECK(strcmp(jr_tools_board_project(), "igd.ops-2026_q3") == 0, "and stored");
    CHECK(build("delegated_tasks", "{}") == JR_TOOL_TEMPLATE_OK, "builds with it");
    CHECK(starts(g_out, "const PJ=\"igd.ops-2026_q3\";"), "the prelude carries the paired project");

    CHECK(!jr_tools_set_board_project("desk\";globalThis.x=1;//"), "a quote is refused");
    CHECK(!jr_tools_set_board_project("two words"), "a space is refused");
    CHECK(strcmp(jr_tools_board_project(), "igd.ops-2026_q3") == 0, "a refusal keeps the old value");
    char longid[60];
    memset(longid, 'a', sizeof longid - 1U);
    longid[sizeof longid - 1U] = '\0';
    CHECK(!jr_tools_set_board_project(longid), "49+ is refused");
    longid[48] = '\0';
    CHECK(jr_tools_set_board_project(longid), "48 is the cap");

    CHECK(jr_tools_set_board_project(""), "empty restores the default");
    CHECK(strcmp(jr_tools_board_project(), "jarvisnano-desk") == 0, "which is jarvisnano-desk");
    CHECK(build("recall_memory", "{\"query\":\"x\"}") == JR_TOOL_TEMPLATE_OK, "a non-board tool builds");
    CHECK(!starts(g_out, "const PJ="), "and carries no prelude");
    return 0;
}

int main(void)
{
    char request_id[JR_TOOLS_REQUEST_ID_CAP];
    CHECK(jr_tools_build_request_id(0xa1b2c3d4U, 0x10203040U, 0x1234abcdU,
                                    "call/with spaces", request_id, sizeof request_id),
          "request id builds");
    CHECK(strcmp(request_id, "g:a1b2c3d4:10203040:1234abcd:call_with_spaces") == 0,
          "request id shape");
    CHECK(!jr_tools_build_request_id(0U, 7U, 1U, "call-1", request_id, sizeof request_id),
          "a zero nonce is refused");

    if (test_fixed_templates() || test_execute_tool_policy() || test_invalid_arguments() ||
        test_delegate_task_is_one_work_item() || test_delegated_tasks_and_poll_are_projected() ||
        test_board_project_is_a_literal_not_a_program()) {
        return 1;
    }
    printf("jr_tools template tests passed (%d checks)\n", g_checks);
    return 0;
}
