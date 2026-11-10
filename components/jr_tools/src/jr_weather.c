/* SPDX-License-Identifier: Apache-2.0 */
#include "jr_tools/jr_weather.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define PP_UNKNOWN 255

const char *jr_weather_condition(int code)
{
    switch (code) {
    case 0:  return "CLEAR";
    case 1:  return "MAINLY CLEAR";
    case 2:  return "PARTLY CLOUDY";
    case 3:  return "OVERCAST";
    case 45: return "FOG";
    case 48: return "RIME FOG";
    case 51: return "LIGHT DRIZZLE";
    case 53: return "DRIZZLE";
    case 55: return "HEAVY DRIZZLE";
    case 56:
    case 57: return "FREEZING DRIZZLE";
    case 61: return "LIGHT RAIN";
    case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66:
    case 67: return "FREEZING RAIN";
    case 71: return "LIGHT SNOW";
    case 73: return "SNOW";
    case 75: return "HEAVY SNOW";
    case 77: return "SNOW GRAINS";
    case 80: return "LIGHT SHOWERS";
    case 81: return "SHOWERS";
    case 82: return "HEAVY SHOWERS";
    case 85:
    case 86: return "SNOW SHOWERS";
    case 95:
    case 96:
    case 99: return "THUNDERSTORM";
    default: return "UNKNOWN";
    }
}

static const cJSON *member(const cJSON *obj, const char *key)
{
    return cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
}

/* First element of an array member, or NULL. */
static const cJSON *first_of(const cJSON *obj, const char *key)
{
    const cJSON *a = member(obj, key);
    return cJSON_IsArray(a) ? cJSON_GetArrayItem(a, 0) : NULL;
}

/* "YYYY-MM-DDTHH:MM" -> minutes past midnight, -1 if it is anything else. */
static int minutes_of(const cJSON *iso)
{
    if (!cJSON_IsString(iso) || iso->valuestring == NULL ||
        strlen(iso->valuestring) < 16U) {
        return -1;
    }
    const char *s = iso->valuestring;
    if (s[10] != 'T' || s[13] != ':') {
        return -1;
    }
    const char d[4] = { s[11], s[12], s[14], s[15] };
    for (int i = 0; i < 4; ++i) {
        if (d[i] < '0' || d[i] > '9') {
            return -1;
        }
    }
    const int h = (d[0] - '0') * 10 + (d[1] - '0');
    const int m = (d[2] - '0') * 10 + (d[3] - '0');
    return h < 24 && m < 60 ? h * 60 + m : -1;
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int full;
} sink_t;

static void put(sink_t *s, const char *fmt, ...)
{
    if (s->full) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= s->cap - s->len) {
        s->full = 1;
        return;
    }
    s->len += (size_t)n;
}

/* A number member as ",\"key\":value" when present. */
static void put_num(sink_t *s, const char *key, const cJSON *v, const char *fmt)
{
    if (cJSON_IsNumber(v)) {
        put(s, ",\"%s\":", key);
        put(s, fmt, v->valuedouble);
    }
}

jr_weather_status_t jr_weather_project(const char *body, size_t len,
                                       char *out, size_t out_cap)
{
    if (out != NULL && out_cap > 0U) {
        out[0] = '\0';
    }
    if (body == NULL || out == NULL || out_cap == 0U) {
        return JR_WEATHER_BAD_ARGS;
    }
    if (len > JR_WEATHER_BODY_CAP) {
        return JR_WEATHER_TOO_LARGE;
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return JR_WEATHER_BAD_JSON;
    }
    const cJSON *cur = member(root, "current");
    const cJSON *t = member(cur, "temperature_2m");
    if (!cJSON_IsNumber(t)) {
        cJSON_Delete(root);
        return JR_WEATHER_NO_CURRENT;
    }
    const cJSON *daily = member(root, "daily");
    const cJSON *code = member(cur, "weather_code");
    const cJSON *pp = member(member(root, "hourly"), "precipitation_probability");

    sink_t s = { .buf = out, .cap = out_cap };
    put(&s, "{\"t\":%.1f", t->valuedouble);
    put_num(&s, "f", member(cur, "apparent_temperature"), "%.1f");
    put_num(&s, "h", member(cur, "relative_humidity_2m"), "%.0f");
    put(&s, ",\"c\":\"%s\"",
        jr_weather_condition(cJSON_IsNumber(code) ? (int)code->valuedouble : -1));
    put_num(&s, "ws", member(cur, "wind_speed_10m"), "%.1f");
    put_num(&s, "hi", first_of(daily, "temperature_2m_max"), "%.1f");
    put_num(&s, "lo", first_of(daily, "temperature_2m_min"), "%.1f");
    put_num(&s, "r", first_of(daily, "precipitation_sum"), "%.1f");
    put(&s, ",\"sr\":%d,\"ss\":%d",
        minutes_of(first_of(daily, "sunrise")), minutes_of(first_of(daily, "sunset")));
    if (cJSON_IsArray(pp) && cJSON_GetArraySize(pp) > 0) {
        put(&s, ",\"pp\":[");
        unsigned k = 0U;
        const cJSON *v = NULL;
        cJSON_ArrayForEach(v, pp) {
            if (k >= JR_WEATHER_HOURS) {
                break;
            }
            const int p = cJSON_IsNumber(v) && v->valuedouble >= 0.0 &&
                          v->valuedouble <= 100.0 ? (int)(v->valuedouble + 0.5)
                                                  : PP_UNKNOWN;
            put(&s, k == 0U ? "%d" : ",%d", p);
            ++k;
        }
        put(&s, "]");
    }
    put(&s, "}");
    cJSON_Delete(root);
    if (s.full) {
        out[0] = '\0';
        return JR_WEATHER_OUT_FULL;
    }
    return JR_WEATHER_OK;
}
