/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_transport/gemini_device_ws.h — DEVICE-PHASE STUB (out of scope for Run 3).
 *
 * The device implementation of the lower ws_transport seam: a thin adapter over
 * esp_websocket_client that plugs UNDER the host-tested Gemini framer/parser
 * (gemini_live.c). It is intentionally NOT compiled by the host suite and NOT
 * yet wired into the device build — the framer/parser above it are already
 * proven by host/test_transport.c against a fake ws_transport. Completing this
 * adapter (chiefly the WEBSOCKET_EVENT_DATA -> recv ring) is a Phase-1 DEVICE
 * acceptance step; do NOT treat it as done. See gemini_live.h for the seam.
 */
#ifndef JR_TRANSPORT_GEMINI_DEVICE_WS_H
#define JR_TRANSPORT_GEMINI_DEVICE_WS_H

#include "jr_ports/ws_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque device state (holds the esp_websocket_client handle + recv ring). */
typedef struct jr_gemini_device_ws jr_gemini_device_ws_t;

/* Build a ws_transport backed by esp_websocket_client. DEVICE-PHASE STUB. */
jr_ws_transport_t jr_gemini_device_ws_make(jr_gemini_device_ws_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* JR_TRANSPORT_GEMINI_DEVICE_WS_H */
