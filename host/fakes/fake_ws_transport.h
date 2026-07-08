/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/fakes/fake_ws_transport.h — the lower-seam ws_transport test double.
 *
 * On device the ws_transport is an esp_websocket_client adapter; on host it is
 * this scriptable fake, which simulates would-block, partial writes, soft
 * failures, a hard close, and a scripted inbox of inbound server frames — so the
 * Gemini framer/parser is exercised with NO socket and NO network. This is the
 * seam that lets v4's worst bug (a would-block sinking the WebSocket) be proven
 * dead in a laptop unit test.
 */
#ifndef HOST_FAKE_WS_TRANSPORT_H
#define HOST_FAKE_WS_TRANSPORT_H

#include "jr_ports/ws_transport.h"

/* Scripted send behaviour (flip between calls to model a socket that fills then
 * drains). */
typedef enum {
    FWS_OK = 0,       /* accept the whole frame                              */
    FWS_WOULD_BLOCK,  /* poll_write==0 / errno==0 — backpressure             */
    FWS_PARTIAL,      /* accept `partial_bytes` then report PARTIAL          */
    FWS_SOFT_FAIL,    /* send error, socket still OPEN (dead-uplink counter) */
    FWS_CLOSED,       /* socket reported closed — the death path             */
} fws_send_mode_t;

#define FWS_SENTLOG_CAP 65536
#define FWS_INBOX_CAP   64

typedef struct {
    jr_ws_state_t   state;
    fws_send_mode_t send_mode;
    size_t          partial_bytes;   /* for FWS_PARTIAL */

    char     sent_log[FWS_SENTLOG_CAP]; /* concatenation of ACCEPTED bytes */
    size_t   sent_log_len;
    unsigned send_calls;
    unsigned close_calls;

    const char *inbox[FWS_INBOX_CAP];   /* scripted inbound frames (FIFO) */
    size_t   inbox_head, inbox_count;
    uint64_t last_rx;
} fake_ws_t;

void   fake_ws_init(fake_ws_t *f);
/* Returns a jr_ws_transport_t whose ctx is `f` (must outlive the transport). */
jr_ws_transport_t fake_ws_make(fake_ws_t *f);
/* Queue one inbound server frame (borrowed pointer; must outlive the recv). */
void   fake_ws_push_inbox(fake_ws_t *f, const char *json);
/* True if the accepted-bytes log contains `needle`. */
int    fake_ws_sent_contains(const fake_ws_t *f, const char *needle);

#endif /* HOST_FAKE_WS_TRANSPORT_H */
