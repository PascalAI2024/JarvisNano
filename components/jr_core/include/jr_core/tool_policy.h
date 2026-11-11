/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/tool_policy.h - which tools the model is offered, and what it is told
 * it has, as a function of whether the company gateway can actually be used.
 *
 * The device declares eleven tools. Seven of them are only a way to reach the
 * gateway (memory, search, the work board, delegation). A device with no
 * gateway key, or one whose gateway does not resolve, used to declare them all
 * and say in its system text that they were its hands; so the model offered to
 * "delegate a task to an agent" (2026-11-10) and every call it made came back
 * as an error. Now an unusable gateway means those tools are not declared, the
 * board is not polled, and the system text says plainly what the device does
 * have.
 *
 * Pure: names in, booleans and text out. Never holds a host name, URL or key.
 */
#ifndef JR_CORE_TOOL_POLICY_H
#define JR_CORE_TOOL_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True for a tool whose only implementation is a call through the gateway.
 * current_time, weather_here, set_volume, set_brightness and ask_user are
 * answered on the device and return false. An unknown name returns true: a
 * tool nobody classified must not be offered without a gateway. */
bool jr_tool_needs_gateway(const char *name);

/* ---- is the gateway usable? --------------------------------------------- */

#define JR_GW_FAIL_LIMIT        3u        /* consecutive network failures = unreachable */
#define JR_GW_POLL_MS_NORMAL    90000u    /* board poll cadence, reachable */
#define JR_GW_POLL_MS_DEGRADED  900000u   /* one probe per 15 min when it is not */

typedef struct {
    uint8_t  net_fails;      /* consecutive failures that never reached a server */
    uint32_t reached_total;
    uint32_t unreached_total;
} jr_gateway_t;

void jr_gateway_init(jr_gateway_t *g);

/* After every call that went to the gateway: reached = a server answered at
 * all (any HTTP status), not reached = DNS, connect or TLS failed. */
void jr_gateway_note(jr_gateway_t *g, bool reached);

/* configured = a gateway address and a key are both stored. Usable also needs
 * the last few calls to have reached something. */
bool jr_gateway_usable(const jr_gateway_t *g, bool configured);

uint32_t jr_gateway_poll_ms(const jr_gateway_t *g);

/* One word for the cockpit: "ok", "worker_not_ready", "gateway_not_configured"
 * or "gateway_unreachable". Checked in that order of blame. */
const char *jr_gateway_why(const jr_gateway_t *g, bool configured,
                           bool worker_ready);

/* ---- what the model is told it has -------------------------------------- */

/* The tools part of the system instruction. `place` is the name of the place
 * the device's own weather is for. Returns the length written (excluding the
 * NUL), or 0 if it did not fit. */
size_t jr_tool_instruction(bool gateway, const char *place, char *out,
                           size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_TOOL_POLICY_H */
