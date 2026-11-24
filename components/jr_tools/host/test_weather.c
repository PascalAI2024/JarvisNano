/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The Open-Meteo projection, pinned against a real response
 * (fixtures/open_meteo_fll.json, fetched with JR_WEATHER_URL on 2026-11-09).
 * Every check compares what the device's weather_apply_result will read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "jr_tools/jr_weather.h"

#define WCHECK(cond, what)                                                    \
    do {                                                                      \
        (*checks)++;                                                          \
        if (!(cond)) {                                                        \
            printf("FAIL weather line %d: %s\n", __LINE__, what);             \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    char *buf = malloc(JR_WEATHER_BODY_CAP * 4U);
    *len = buf != NULL ? fread(buf, 1U, JR_WEATHER_BODY_CAP * 4U - 1U, f) : 0U;
    fclose(f);
    if (buf != NULL) {
        buf[*len] = '\0';
    }
    return buf;
}

static double num(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : -12345.0;
}

static int near(double a, double b) { return a - b < 0.001 && b - a < 0.001; }

static jr_weather_status_t project_str(const char *json, char *out, size_t cap)
{
    return jr_weather_project(json, strlen(json), out, cap);
}

int weather_tests(int *checks)
{
    size_t len = 0U;
    char *body = slurp(FIXTURE_DIR "/open_meteo_fll.json", &len);
    WCHECK(body != NULL && len > 1000U, "the real fixture loads");
    WCHECK(len <= JR_WEATHER_BODY_CAP, "a real response fits the cap");

    char out[1024];
    WCHECK(jr_weather_project(body, len, out, sizeof out) == JR_WEATHER_OK,
           "the real response projects");
    cJSON *r = cJSON_Parse(out);
    WCHECK(r != NULL, "the projection is JSON");
    WCHECK(near(num(r, "t"), 24.0), "t is current.temperature_2m");
    WCHECK(near(num(r, "f"), 25.9), "f is apparent_temperature");
    WCHECK(near(num(r, "h"), 73.0), "h is relative_humidity_2m");
    WCHECK(near(num(r, "ws"), 9.8), "ws is km/h, unconverted");
    WCHECK(near(num(r, "hi"), 28.9), "hi is today's max");
    WCHECK(near(num(r, "lo"), 22.7), "lo is today's min");
    WCHECK(near(num(r, "r"), 0.0), "r is today's precipitation sum");
    WCHECK(near(num(r, "sr"), 6 * 60 + 34), "sunrise 06:34 is 394 minutes");
    WCHECK(near(num(r, "ss"), 17 * 60 + 33), "sunset 17:33 is 1053 minutes");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(r, "c");
    WCHECK(cJSON_IsString(c) && strcmp(c->valuestring, "CLEAR") == 0,
           "weather_code 0 reads CLEAR");
    static const int expect_pp[36] = {
        0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 3, 3,
        3, 4, 2, 2, 0, 0, 0, 1, 3, 8, 7, 2, 3, 7, 11, 20, 14, 10 };
    const cJSON *pp = cJSON_GetObjectItemCaseSensitive(r, "pp");
    WCHECK(cJSON_IsArray(pp) && cJSON_GetArraySize(pp) == 36,
           "pp is the first 36 of the 48 hours");
    int pp_ok = 1;
    for (int i = 0; i < 36; ++i) {
        const cJSON *v = cJSON_GetArrayItem(pp, i);
        pp_ok = pp_ok && cJSON_IsNumber(v) && (int)v->valuedouble == expect_pp[i];
    }
    WCHECK(pp_ok, "pp carries the hourly probabilities in order");
    cJSON_Delete(r);
    free(body);

    /* Failure modes. */
    WCHECK(project_str("{\"daily\":{}}", out, sizeof out) == JR_WEATHER_NO_CURRENT,
           "no current is an error");
    WCHECK(out[0] == '\0', "and leaves the output empty");
    WCHECK(project_str("{\"current\":{\"weather_code\":0}}", out, sizeof out) ==
               JR_WEATHER_NO_CURRENT, "current without a temperature is an error");
    WCHECK(project_str("{\"current\":{\"temperature_2m\":null}}", out, sizeof out) ==
               JR_WEATHER_NO_CURRENT, "a null temperature is an error");
    WCHECK(project_str("not json", out, sizeof out) == JR_WEATHER_BAD_JSON,
           "garbage is bad json");
    WCHECK(project_str("[1]", out, sizeof out) == JR_WEATHER_BAD_JSON,
           "a non-object root is bad json");
    WCHECK(jr_weather_project(NULL, 0U, out, sizeof out) == JR_WEATHER_BAD_ARGS,
           "no body is bad args");

    /* null hourly values read 255, sunrise absent reads -1. */
    WCHECK(project_str("{\"current\":{\"temperature_2m\":10.5},"
                       "\"hourly\":{\"precipitation_probability\":[null,40,null]}}",
                       out, sizeof out) == JR_WEATHER_OK, "sparse response projects");
    r = cJSON_Parse(out);
    WCHECK(r != NULL, "sparse projection is JSON");
    pp = cJSON_GetObjectItemCaseSensitive(r, "pp");
    WCHECK(cJSON_GetArraySize(pp) == 3 &&
               (int)cJSON_GetArrayItem(pp, 0)->valuedouble == 255 &&
               (int)cJSON_GetArrayItem(pp, 1)->valuedouble == 40 &&
               (int)cJSON_GetArrayItem(pp, 2)->valuedouble == 255,
           "null hourly values read 255");
    WCHECK(near(num(r, "sr"), -1.0) && near(num(r, "ss"), -1.0),
           "absent sunrise and sunset read -1");
    WCHECK(cJSON_GetObjectItemCaseSensitive(r, "f") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "hi") == NULL,
           "absent optional fields are left out, not zero");
    const cJSON *c2 = cJSON_GetObjectItemCaseSensitive(r, "c");
    WCHECK(cJSON_IsString(c2) && strcmp(c2->valuestring, "UNKNOWN") == 0,
           "no weather_code reads UNKNOWN");
    cJSON_Delete(r);

    WCHECK(project_str("{\"current\":{\"temperature_2m\":1},"
                       "\"daily\":{\"sunrise\":[\"2026-11-09T6:34\"],"
                       "\"sunset\":[\"2026-11-09T25:00\"]}}", out, sizeof out) ==
               JR_WEATHER_OK, "malformed sun times still project");
    r = cJSON_Parse(out);
    WCHECK(near(num(r, "sr"), -1.0) && near(num(r, "ss"), -1.0),
           "malformed sun times read -1");
    cJSON_Delete(r);

    /* An oversized body is rejected, not truncated. */
    char *big = malloc(JR_WEATHER_BODY_CAP + 64U);
    WCHECK(big != NULL, "big buffer");
    memset(big, ' ', JR_WEATHER_BODY_CAP + 32U);
    memcpy(big, "{\"current\":{\"temperature_2m\":1}}", 32U);
    WCHECK(jr_weather_project(big, JR_WEATHER_BODY_CAP, out, sizeof out) ==
               JR_WEATHER_OK, "a body at the cap is accepted");
    WCHECK(jr_weather_project(big, JR_WEATHER_BODY_CAP + 1U, out, sizeof out) ==
               JR_WEATHER_TOO_LARGE, "a body one byte over the cap is rejected");
    free(big);

    /* A small output buffer fails closed. */
    WCHECK(project_str("{\"current\":{\"temperature_2m\":1}}", out, 20U) ==
               JR_WEATHER_OUT_FULL && out[0] == '\0',
           "a short output buffer is an error, never a cut-off object");

    /* The condition words reach the right sky in the device's substring
     * mapper (main/device_tools.c weather_sky_from): thunder/storm, then
     * snow/sleet/ice, then rain/drizzle/shower, fog, overcast, cloud, clear. */
    WCHECK(strcmp(jr_weather_condition(2), "PARTLY CLOUDY") == 0, "code 2");
    WCHECK(strcmp(jr_weather_condition(3), "OVERCAST") == 0, "code 3");
    WCHECK(strcmp(jr_weather_condition(45), "FOG") == 0, "code 45");
    WCHECK(strcmp(jr_weather_condition(51), "LIGHT DRIZZLE") == 0, "code 51");
    WCHECK(strcmp(jr_weather_condition(61), "LIGHT RAIN") == 0, "code 61");
    WCHECK(strcmp(jr_weather_condition(65), "HEAVY RAIN") == 0, "code 65");
    WCHECK(strcmp(jr_weather_condition(81), "SHOWERS") == 0, "code 81");
    WCHECK(strcmp(jr_weather_condition(86), "SNOW SHOWERS") == 0, "code 86");
    WCHECK(strcmp(jr_weather_condition(95), "THUNDERSTORM") == 0, "code 95");
    WCHECK(strcmp(jr_weather_condition(1234), "UNKNOWN") == 0, "unlisted code");
    return 0;
}
