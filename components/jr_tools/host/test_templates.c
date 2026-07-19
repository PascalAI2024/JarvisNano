/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jr_tools/jr_tools.h"

static void expect_ok(const char *name, const char *args, const char *expected)
{
    char out[768];
    assert(jr_tools_build_code(name, args, out, sizeof(out)) ==
           JR_TOOL_TEMPLATE_OK);
    assert(strcmp(out, expected) == 0);
}

int main(void)
{
    char request_id[JR_TOOLS_REQUEST_ID_CAP];
    assert(jr_tools_build_request_id(0xa1b2c3d4U, 0x10203040U,
                                     0x1234abcdU, "call/with spaces",
                                     request_id, sizeof(request_id)));
    assert(strcmp(request_id,
                  "g:a1b2c3d4:10203040:1234abcd:call_with_spaces") == 0);
    char repeated[JR_TOOLS_REQUEST_ID_CAP];
    assert(jr_tools_build_request_id(0xa1b2c3d4U, 7U, 0U,
                                     "provider-call-42", repeated,
                                     sizeof(repeated)));
    assert(jr_tools_build_request_id(0xa1b2c3d4U, 7U, 0U,
                                     "provider-call-42", request_id,
                                     sizeof(request_id)));
    assert(strcmp(request_id, repeated) == 0);
    assert(strlen(request_id) <= 96U);
    assert(jr_tools_build_request_id(0xa1b2c3d4U, 8U, 0U,
                                     "provider-call-42", request_id,
                                     sizeof(request_id)));
    assert(strcmp(request_id, repeated) != 0);
    assert(jr_tools_build_request_id(0xa1b2c3d5U, 7U, 0U,
                                     "provider-call-42", request_id,
                                     sizeof(request_id)));
    assert(strcmp(request_id, repeated) != 0);
    char long_id[160];
    memset(long_id, 'x', sizeof(long_id) - 1U);
    long_id[sizeof(long_id) - 1U] = '\0';
    assert(jr_tools_build_request_id(0xa1b2c3d4U, 7U, 1U, long_id,
                                     request_id, sizeof(request_id)));
    assert(strlen(request_id) == 96U);
    assert(!jr_tools_build_request_id(0U, 7U, 1U, "call-1", request_id,
                                      sizeof(request_id)));
    assert(!jr_tools_build_request_id(0xa1b2c3d4U, 7U, 0U, NULL, request_id,
                                      sizeof(request_id)));

    expect_ok("crypto_price", "{}",
              "return await jarvis.crypto(\"bitcoin\")");
    expect_ok("crypto_price", "{\"symbol\":\"BTC-USD\"}",
              "return await jarvis.crypto(\"BTC-USD\")");
    expect_ok("recall_memory", "{\"query\":\"release plan\"}",
              "return await jarvis.memory.search({query:\"release plan\",area:\"all\",limit:5})");
    expect_ok("remember", "{\"note\":\"Pascal's tea, (green & hot)\"}",
              "const b=\"Pascal's tea, (green & hot)\";"
              "const h=await jarvis.utils.hash(b);"
              "return await jarvis.memory.capture({type:\"note\","
              "title:\"JarvisNano \"+h.slice(0,12),body:b,"
              "tags:[\"jarvisnano\",\"voice\"],source:\"jarvisnano\","
              "confidence:\"confirmed\"})");
    expect_ok("wikipedia", "{\"topic\":\"Ada Lovelace\"}",
              "const r=await jarvis.wiki(\"Ada Lovelace\");"
              "if(Array.isArray(r.results))return {query:r.query,"
              "results:r.results.slice(0,3).map(x=>({title:x.title,"
              "description:x.description,extract:(x.extract||\"\").slice(0,400),"
              "url:x.url}))};return {title:r.title,description:r.description,"
              "extract:(r.extract||\"\").slice(0,1800),url:r.url}");
    expect_ok("country_info", "{\"country\":\"United Kingdom\"}",
              "return await jarvis.country(\"United Kingdom\")");
    expect_ok("weather", "{\"location\":\"Fort Lauderdale, FL\"}",
              "const g=await jarvis.geocode(\"Fort Lauderdale, FL\",{limit:1});"
              "if(!g.results?.length)return {error:\"Location not found\"};"
              "return await jarvis.weather(g.results[0].lat,g.results[0].lon,{days:3})");
    expect_ok("current_time", "{}",
              "return await jarvis.time(\"America/New_York\")");
    expect_ok("current_time", "{\"timezone\":\"Europe/London\"}",
              "return await jarvis.time(\"Europe/London\")");

    /* A memory consent must be exactly representable on the panel. */
    char out[768];
    assert(jr_tools_build_code("remember",
           "{\"note\":\"x\\\");globalThis.pwned=true;//\"}", out,
           sizeof(out)) == JR_TOOL_TEMPLATE_INVALID_ARGS);
    assert(jr_tools_build_code("remember", "{\"note\":\"save café\"}", out,
                               sizeof(out)) == JR_TOOL_TEMPLATE_INVALID_ARGS);

    assert(jr_tools_build_code("ask_jarvis", "{\"code\":\"return 1\"}",
                               out, sizeof(out)) ==
           JR_TOOL_TEMPLATE_UNKNOWN_TOOL);
    assert(jr_tools_build_code("remember", "[]", out, sizeof(out)) ==
           JR_TOOL_TEMPLATE_INVALID_ARGS);
    assert(jr_tools_build_code("remember", "{}", out, sizeof(out)) ==
           JR_TOOL_TEMPLATE_INVALID_ARGS);
    assert(jr_tools_build_code("crypto_price", "{\"symbol\":\"BTC');boom()\"}",
                               out, sizeof(out)) ==
           JR_TOOL_TEMPLATE_INVALID_ARGS);
    assert(jr_tools_build_code("wikipedia", "{broken", out, sizeof(out)) ==
           JR_TOOL_TEMPLATE_INVALID_ARGS);
    assert(jr_tools_build_code("wikipedia",
                               "{\"topic\":\"Ada\"} trailing", out,
                               sizeof(out)) == JR_TOOL_TEMPLATE_INVALID_ARGS);
    assert(jr_tools_build_code("wikipedia",
                               "{\"topic\":\"Ada\",\"code\":\"return 1\"}",
                               out, sizeof(out)) == JR_TOOL_TEMPLATE_INVALID_ARGS);
    assert(jr_tools_build_code("remember",
                               "{\"note\":\"approved\",\"note\":\"swapped\"}",
                               out, sizeof(out)) == JR_TOOL_TEMPLATE_INVALID_ARGS);
    assert(jr_tools_build_code(
               "remember",
               "{\"note\":\"123456789012345678901234567890123456789012345678\"}",
               out, sizeof(out)) == JR_TOOL_TEMPLATE_INVALID_ARGS);

    char tiny[8];
    assert(jr_tools_build_code("current_time", "{}", tiny, sizeof(tiny)) ==
           JR_TOOL_TEMPLATE_TOO_LARGE);

    puts("jr_tools template tests passed");
    return 0;
}
