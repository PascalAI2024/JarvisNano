/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_transport/gemini_device_ws.h — the DEVICE impl of the lower ws_transport
 * seam: a thin adapter over esp_websocket_client that plugs UNDER the
 * host-tested Gemini framer/parser (gemini_live.c). It buries v4's worst bug —
 * a transient would-block that aborted the whole socket — behind the port by
 * classifying esp_websocket_client's poll_write==0 as JR_WS_TX_WOULD_BLOCK
 * (backpressure), never a teardown. Inbound frames arrive via the
 * WEBSOCKET_EVENT_DATA handler and are reassembled into a recv ring that
 * recv_frame() pops.
 *
 * Module-singleton (one WSS session per device): jr_gemini_ws_init(url) builds
 * the client + recv ring; jr_gemini_ws() returns the jr_ws_transport_t view to
 * inject under jr_gemini_client_init(). The URL carries the API key as a query
 * param at runtime — NEVER hardcode it in-repo.
 */
#ifndef JR_TRANSPORT_GEMINI_DEVICE_WS_H
#define JR_TRANSPORT_GEMINI_DEVICE_WS_H

#include "esp_err.h"
#include "jr_ports/ws_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the recv ring and remember the default endpoint URL (the full wss
 * URL including ?key=). Idempotent. connect() may also receive a URL from the
 * framer's cfg.url; a non-empty one there wins, else this default is used. */
esp_err_t jr_gemini_ws_init(const char *url);

/* Extra HTTP headers for the WS upgrade request — full "Name: value\r\n"
 * lines, copied. NULL or "" clears. This exists so the API key can ride an
 * x-goog-api-key header instead of the ?key= URL: esp_websocket_client logs
 * its uri on every transport error, and a keyed URL put the key on the serial
 * console and the SD log. The stored buffer therefore holds the key — it must
 * NEVER be logged. Takes effect on the next connect. */
void jr_gemini_ws_set_headers(const char *headers_crlf);

/* The byte-level transport view to inject under the host-tested Gemini client. */
jr_ws_transport_t jr_gemini_ws(void);

#ifdef __cplusplus
}
#endif

#endif /* JR_TRANSPORT_GEMINI_DEVICE_WS_H */
