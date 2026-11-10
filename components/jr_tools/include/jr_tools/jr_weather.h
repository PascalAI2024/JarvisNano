/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_weather — the glass's own weather, straight from Open-Meteo.
 *
 * Open-Meteo is public and keyless, so the WEATHER refresh needs neither the
 * JarvisMCP gateway nor a key. The worker does one HTTPS GET of
 * JR_WEATHER_URL and hands the body to jr_weather_project(), a pure function
 * (no ESP-IDF) that writes the compact JSON the device's weather_apply_result
 * reads: {"t","f","h","c","ws","hi","lo","r","sr","ss","pp":[..]}.
 * Metric in; the device converts.
 */
#ifndef JR_TOOLS_JR_WEATHER_H
#define JR_TOOLS_JR_WEATHER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The device-owned job that is served by this fetch instead of the gateway. */
#define JR_WEATHER_JOB_NAME "weather_glance"

/* Fort Lauderdale, America/New_York. The gateway template of the same job in
 * jr_tools_templates.c carries the same coordinates as JavaScript text; change
 * both together. */
#define JR_WEATHER_URL                                                         \
    "https://api.open-meteo.com/v1/forecast?latitude=26.1224&longitude=-80.1373" \
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"       \
    "weather_code,wind_speed_10m"                                              \
    "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,"          \
    "sunrise,sunset"                                                           \
    "&hourly=precipitation_probability&forecast_days=2"                        \
    "&timezone=America%2FNew_York"

/* A real response measured 2068 bytes on 2026-11-09 (48 hourly values and
 * their ISO times are most of it). Twice that is the cap; a larger body is
 * rejected, not truncated. */
#define JR_WEATHER_BODY_CAP 4096U

/* Hours of precipitation probability kept, from today's local midnight. */
#define JR_WEATHER_HOURS 36U

typedef enum {
    JR_WEATHER_OK = 0,
    JR_WEATHER_BAD_ARGS,
    JR_WEATHER_TOO_LARGE,   /* body longer than JR_WEATHER_BODY_CAP */
    JR_WEATHER_BAD_JSON,
    JR_WEATHER_NO_CURRENT,  /* no current.temperature_2m */
    JR_WEATHER_OUT_FULL,    /* out_cap too small for the projection */
} jr_weather_status_t;

/* Condition words for a WMO weather_code. They are matched by substring by the
 * device's weather_sky_from(), so each word must land on the right sky. */
const char *jr_weather_condition(int wmo_code);

/* Projects an Open-Meteo forecast body (len bytes, need not be terminated)
 * into out as one JSON object. Missing optional fields are left out; sunrise
 * and sunset read -1 when absent; a null hourly value reads 255. On any error
 * out holds an empty string. */
jr_weather_status_t jr_weather_project(const char *body, size_t len,
                                       char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* JR_TOOLS_JR_WEATHER_H */
