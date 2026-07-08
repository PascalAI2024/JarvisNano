/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_transport/gemini_device_ws.c — DEVICE-PHASE STUB. NOT compiled by the host
 * suite; NOT yet in the device build (see components/jr_transport/CMakeLists.txt).
 *
 * This is the ONLY file in the transport that references esp_websocket_client —
 * the lower ws_transport seam's device impl. The host-tested framer/parser
 * (gemini_live.c) sit ABOVE it and are already green; this adapter's correctness
 * is a Phase-1 DEVICE acceptance step. The load-bearing translation it owns:
 * esp_websocket_client_send_text()'s poll_write==0 / errno==0 case maps to
 * JR_WS_TX_WOULD_BLOCK (backpressure) — NEVER a teardown. That is what buries
 * v4's write-block abort behind our interface (spec §5.6).
 *
 * TODO(device): the send path below mirrors the proven v4 call
 * (cap_gemini_live.c gl_ws_send_text_to -> esp_websocket_client_send_text). The
 * recv path is the remaining work: esp_websocket_client delivers inbound frames
 * via a WEBSOCKET_EVENT_DATA handler, not a poll — feed a FreeRTOS ring from
 * that handler and pop it in dev_recv_frame(). Until then dev_recv_frame() is a
 * stub returning 0. Intended wiring at L6:
 *     jr_gemini_device_ws_t dev = {...};
 *     jr_ws_transport_t ws = jr_gemini_device_ws_make(&dev);
 *     jr_gemini_client_t c; jr_gemini_client_init(&c, ws, esp_clock, &cfg);
 *     jr_realtime_voice_client_t rvc = jr_gemini_client_as_rvc(&c);  // upper seam
 */
#include "jr_transport/gemini_device_ws.h"

#include "esp_websocket_client.h"
#include "esp_timer.h"

struct jr_gemini_device_ws {
    esp_websocket_client_handle_t client;
    uint64_t last_rx_ms;
    /* TODO(device): recv ring fed by the WEBSOCKET_EVENT_DATA handler. */
};

static jr_err_t dev_connect(void *ctx, const char *url)
{
    (void)url;   /* TODO(device): build esp_websocket_client_config_t{ .uri=url },
                  * esp_websocket_client_init + register the event handler that
                  * feeds the recv ring, then esp_websocket_client_start(). */
    (void)ctx;
    return JR_ERR_FAIL;   /* STUB */
}

static jr_ws_send_result_t dev_send_bytes(void *ctx, const uint8_t *buf, size_t len)
{
    struct jr_gemini_device_ws *d = (struct jr_gemini_device_ws *)ctx;
    jr_ws_send_result_t r = { JR_WS_TX_CLOSED, 0 };
    if (!d || !d->client) {
        return r;
    }
    /* Non-blocking send (timeout 0): the proven v4 API. */
    int sent = esp_websocket_client_send_text(d->client, (const char *)buf,
                                              (int)len, 0);
    if (sent == (int)len) {
        r.kind = JR_WS_TX_OK;
        r.sent = (size_t)sent;
    } else if (sent > 0) {
        r.kind = JR_WS_TX_PARTIAL;
        r.sent = (size_t)sent;
    } else if (sent == 0) {
        /* poll_write timed out with the socket still up: BACKPRESSURE, not a
         * teardown. THE fix — never abort here. */
        r.kind = JR_WS_TX_WOULD_BLOCK;
    } else if (esp_websocket_client_is_connected(d->client)) {
        r.kind = JR_WS_TX_SOFT_FAIL;   /* error but socket open -> dead-uplink count */
    } else {
        r.kind = JR_WS_TX_CLOSED;      /* the death path */
    }
    return r;
}

static int dev_recv_frame(void *ctx, char *buf, size_t cap)
{
    (void)ctx; (void)buf; (void)cap;
    return 0;   /* STUB — TODO(device): pop one frame from the WS-event ring. */
}

static jr_ws_state_t dev_state(void *ctx)
{
    struct jr_gemini_device_ws *d = (struct jr_gemini_device_ws *)ctx;
    if (!d || !d->client) {
        return JR_WS_CLOSED;
    }
    return esp_websocket_client_is_connected(d->client) ? JR_WS_OPEN : JR_WS_CLOSED;
}

static uint64_t dev_last_rx_ms(void *ctx)
{
    struct jr_gemini_device_ws *d = (struct jr_gemini_device_ws *)ctx;
    return d ? d->last_rx_ms : 0;
}

static void dev_close(void *ctx)
{
    struct jr_gemini_device_ws *d = (struct jr_gemini_device_ws *)ctx;
    if (d && d->client) {
        esp_websocket_client_stop(d->client);
        esp_websocket_client_destroy(d->client);
        d->client = NULL;
    }
}

jr_ws_transport_t jr_gemini_device_ws_make(jr_gemini_device_ws_t *dev)
{
    jr_ws_transport_t t;
    t.ctx = dev;
    t.connect = dev_connect;
    t.send_bytes = dev_send_bytes;
    t.recv_frame = dev_recv_frame;
    t.state = dev_state;
    t.last_rx_ms = dev_last_rx_ms;
    t.close = dev_close;
    return t;
}
