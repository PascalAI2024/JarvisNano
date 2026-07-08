/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/ws_transport.h — the LOWER-SEAM byte transport port (L2).
 *
 * This is the seam that makes the Gemini framer host-testable WITHOUT a socket.
 * The RealtimeVoiceClient Gemini impl (builder / framer / parser) sits ON TOP of
 * this port. On device a thin adapter wraps `esp_websocket_client`; on host a
 * fake (host/fakes/fake_ws_transport.c) simulates would-block, partial writes,
 * and scripted inbound server frames.
 *
 * THE ONE RULE (architecture.md): nothing under jr_ports may include an ESP-IDF,
 * driver, network, or model header — only the vocabulary in jr_types.h. The
 * device adapter translates esp_websocket_client_send_with_opts()'s
 * poll_write==0 / errno==0 into JR_WS_TX_WOULD_BLOCK here, so the un-patchable
 * write-0 abort behaviour is buried behind this interface (v4's worst bug: a
 * transient would-block aborted the whole WebSocket — see spec §5.6).
 */
#ifndef JR_PORTS_WS_TRANSPORT_H
#define JR_PORTS_WS_TRANSPORT_H

#include "jr_ports/jr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connection state the framer polls to decide death vs backpressure. */
typedef enum {
    JR_WS_CLOSED = 0,   /* not connected / cleanly closed                     */
    JR_WS_CONNECTING,   /* connect() in flight                                */
    JR_WS_OPEN,         /* usable; send/recv allowed                          */
    JR_WS_ERROR,        /* hard transport error — the death path              */
} jr_ws_state_t;

/* The three outcomes of ONE byte-level send attempt (the load-bearing
 * distinction of §5.6). The device adapter classifies esp_websocket_client's
 * return this way; the host fake scripts it directly. */
typedef enum {
    JR_WS_TX_OK = 0,        /* all `len` bytes accepted                       */
    JR_WS_TX_PARTIAL,       /* 0 < sent < len — remainder must be buffered    */
    JR_WS_TX_WOULD_BLOCK,   /* poll_write==0 / errno==0 — BACKPRESSURE, NOT an
                             * error. Socket stays up. NEVER teardown.        */
    JR_WS_TX_SOFT_FAIL,     /* send returned an error but the socket is still
                             * OPEN (v4's tx_send_failures=175 case). Feeds the
                             * DeadUplinkMonitor; a single one is NOT a death. */
    JR_WS_TX_CLOSED,        /* socket reported closed/errored — immediate death*/
} jr_ws_tx_kind_t;

typedef struct {
    jr_ws_tx_kind_t kind;
    size_t          sent;   /* bytes accepted (meaningful for OK / PARTIAL)   */
} jr_ws_send_result_t;

/* Byte-level WebSocket transport. Whole-frame semantics on recv (a WS text
 * frame == one message); the framer owns outbound buffering above this. */
typedef struct jr_ws_transport {
    void *ctx;

    /* Begin an async connect. State transitions CONNECTING -> OPEN / ERROR. */
    jr_err_t (*connect)(void *ctx, const char *url);

    /* Attempt to send `len` bytes of one text frame. Non-blocking: returns a
     * classified result; NEVER blocks the caller and NEVER tears the socket
     * down on would-block. */
    jr_ws_send_result_t (*send_bytes)(void *ctx, const uint8_t *buf, size_t len);

    /* Non-blocking recv of ONE inbound text frame into `buf` (capacity `cap`).
     * Returns bytes copied (0 == nothing available now), or a negative jr_err_t
     * on a hard error. The impl updates its last-rx clock on every inbound byte
     * so `last_rx_ms` is an independent downlink-liveness signal. */
    int (*recv_frame)(void *ctx, char *buf, size_t cap);

    jr_ws_state_t (*state)(void *ctx);

    /* Monotonic ms of the last inbound frame — the downlink-liveness feed the
     * KeepaliveMonitor consumes, independent of the uplink tx-outcome feed. */
    uint64_t (*last_rx_ms)(void *ctx);

    void (*close)(void *ctx);
} jr_ws_transport_t;

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_WS_TRANSPORT_H */
