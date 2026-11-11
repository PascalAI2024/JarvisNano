/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/tool_policy.c - see tool_policy.h.
 */
#include "jr_core/tool_policy.h"

#include <stdio.h>
#include <string.h>

bool jr_tool_needs_gateway(const char *name)
{
    if (name == NULL) {
        return true;
    }
    static const char *const kLocal[] = {
        "current_time", "weather_here", "set_volume", "set_brightness",
        "ask_user",
    };
    for (size_t i = 0; i < sizeof kLocal / sizeof kLocal[0]; ++i) {
        if (strcmp(name, kLocal[i]) == 0) {
            return false;
        }
    }
    return true;
}

void jr_gateway_init(jr_gateway_t *g)
{
    memset(g, 0, sizeof *g);
}

void jr_gateway_note(jr_gateway_t *g, bool reached)
{
    if (reached) {
        g->net_fails = 0u;
        g->reached_total++;
    } else {
        if (g->net_fails < 255u) {
            g->net_fails++;
        }
        g->unreached_total++;
    }
}

bool jr_gateway_usable(const jr_gateway_t *g, bool configured)
{
    return configured && g->net_fails < JR_GW_FAIL_LIMIT;
}

uint32_t jr_gateway_poll_ms(const jr_gateway_t *g)
{
    return g->net_fails < JR_GW_FAIL_LIMIT ? JR_GW_POLL_MS_NORMAL
                                           : JR_GW_POLL_MS_DEGRADED;
}

const char *jr_gateway_why(const jr_gateway_t *g, bool configured,
                           bool worker_ready)
{
    if (!worker_ready) {
        return "worker_not_ready";
    }
    if (!configured) {
        return "gateway_not_configured";
    }
    if (g->net_fails >= JR_GW_FAIL_LIMIT) {
        return "gateway_unreachable";
    }
    return "ok";
}

/* With a gateway: the paragraphs the persona always carried, unchanged, plus
 * the one line that stops the model asking where it is. */
static const char kWithGateway[] =
    "You exist to help with Sir's life and work, and the Jarvis tools are "
    "how you act: remember what he tells you, recall it later, read and "
    "add to his calendar, keep his work board, search the world. Use them "
    "without being asked twice; never claim a tool succeeded unless its "
    "response says so.\n\n"
    "You also have hands elsewhere. delegate_task queues a job for Sir's "
    "agents, who work on their own machines and report back to this "
    "device; delegated_tasks lists what is queued, running, done or "
    "blocked, and it alone answers anything about the board or the "
    "tasks. execute_tool is for a method search_tools found, never for "
    "the board. Anything one tool call answers is answered now: a fact, the "
    "weather, a price, a note. Anything that needs a browser, a "
    "repository, a document, or several steps is delegated in one call, "
    "with Sir told it is queued and that the result will be announced "
    "here when it lands. Nothing that can be answered now is delegated.\n\n"
    "For the weather at Sir's own place, weather_here answers at once with "
    "no arguments: use it, and never ask Sir where he is.\n\n"
    "Capabilities, for when Sir asks what you can do: live web search and "
    "news, weather, Wikipedia, crypto and stock prices, exchange rates, "
    "time zones, translation, research papers, remembering and recalling "
    "Sir's notes, his calendar, his work board, jobs for his agents, and "
    "this device's volume and brightness.";

/* Without one: what the device has, and what it must not offer. */
static const char kNoGateway[] =
    "This device is not connected to Sir's company gateway, so the Jarvis "
    "tools are not available here: you cannot search the web or the news, "
    "remember or recall notes, read his calendar or work board, or hand a "
    "job to his agents. Never offer any of that, never say you will try, "
    "and never offer to delegate.\n\n"
    "What you do have: current_time, the time now; weather_here, the "
    "weather this device holds for its own place, %s, which answers at "
    "once with no arguments (a question about the weather is answered from "
    "it, and you never ask Sir where he is); set_volume and set_brightness "
    "for this device; and ask_user to offer Sir a choice on the glass.\n\n"
    "When Sir asks what you can do, say that, in a sentence. Anything else "
    "you cannot do from here: say so in one short sentence, and answer from "
    "what you know if you can.";

size_t jr_tool_instruction(bool gateway, const char *place, char *out,
                           size_t cap)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    out[0] = '\0';
    const int n = gateway
        ? snprintf(out, cap, "%s", kWithGateway)
        : snprintf(out, cap, kNoGateway,
                   (place != NULL && place[0] != '\0') ? place : "home");
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0u;
    }
    return (size_t)n;
}
