/*
 * SPDX-License-Identifier: Apache-2.0
 * host/fakes/fake_ws_transport.c — scriptable ws_transport double.
 */
#include "fake_ws_transport.h"
#include <string.h>

static void sent_append(fake_ws_t *f, const uint8_t *buf, size_t len)
{
    if (f->sent_log_len + len >= FWS_SENTLOG_CAP) {
        len = FWS_SENTLOG_CAP - 1 - f->sent_log_len;
    }
    memcpy(f->sent_log + f->sent_log_len, buf, len);
    f->sent_log_len += len;
    f->sent_log[f->sent_log_len] = '\0';
}

static jr_err_t fws_connect(void *ctx, const char *url)
{
    (void)url;
    fake_ws_t *f = (fake_ws_t *)ctx;
    f->state = JR_WS_OPEN;
    return JR_OK;
}

static jr_ws_send_result_t fws_send_bytes(void *ctx, const uint8_t *buf, size_t len)
{
    fake_ws_t *f = (fake_ws_t *)ctx;
    f->send_calls++;
    jr_ws_send_result_t r = { JR_WS_TX_OK, 0 };
    switch (f->send_mode) {
    case FWS_OK:
        sent_append(f, buf, len);
        r.kind = JR_WS_TX_OK;
        r.sent = len;
        break;
    case FWS_WOULD_BLOCK:
        r.kind = JR_WS_TX_WOULD_BLOCK;
        r.sent = 0;
        break;
    case FWS_PARTIAL: {
        size_t k = f->partial_bytes;
        if (k >= len) {
            sent_append(f, buf, len);
            r.kind = JR_WS_TX_OK;
            r.sent = len;
        } else {
            sent_append(f, buf, k);
            r.kind = JR_WS_TX_PARTIAL;
            r.sent = k;
        }
        break;
    }
    case FWS_SOFT_FAIL:
        r.kind = JR_WS_TX_SOFT_FAIL;
        r.sent = 0;
        break;
    case FWS_CLOSED:
        f->state = JR_WS_CLOSED;
        r.kind = JR_WS_TX_CLOSED;
        r.sent = 0;
        break;
    }
    return r;
}

static int fws_recv_frame(void *ctx, char *buf, size_t cap)
{
    fake_ws_t *f = (fake_ws_t *)ctx;
    if (f->inbox_count == 0) {
        return 0;
    }
    const char *frame = f->inbox[f->inbox_head];
    f->inbox_head = (f->inbox_head + 1) % FWS_INBOX_CAP;
    f->inbox_count--;
    size_t len = strlen(frame);
    if (len > cap) {
        len = cap;
    }
    memcpy(buf, frame, len);
    f->last_rx++;   /* a monotonic downlink tick per inbound frame */
    return (int)len;
}

static jr_ws_state_t fws_state(void *ctx)
{
    return ((fake_ws_t *)ctx)->state;
}

static uint64_t fws_last_rx_ms(void *ctx)
{
    return ((fake_ws_t *)ctx)->last_rx;
}

static void fws_close(void *ctx)
{
    fake_ws_t *f = (fake_ws_t *)ctx;
    f->close_calls++;
    f->state = JR_WS_CLOSED;
}

void fake_ws_init(fake_ws_t *f)
{
    memset(f, 0, sizeof(*f));
    f->state = JR_WS_CLOSED;
    f->send_mode = FWS_OK;
}

jr_ws_transport_t fake_ws_make(fake_ws_t *f)
{
    jr_ws_transport_t t;
    t.ctx = f;
    t.connect = fws_connect;
    t.send_bytes = fws_send_bytes;
    t.recv_frame = fws_recv_frame;
    t.state = fws_state;
    t.last_rx_ms = fws_last_rx_ms;
    t.close = fws_close;
    return t;
}

void fake_ws_push_inbox(fake_ws_t *f, const char *json)
{
    if (f->inbox_count >= FWS_INBOX_CAP) {
        return;
    }
    size_t tail = (f->inbox_head + f->inbox_count) % FWS_INBOX_CAP;
    f->inbox[tail] = json;
    f->inbox_count++;
}

int fake_ws_sent_contains(const fake_ws_t *f, const char *needle)
{
    return strstr(f->sent_log, needle) != NULL;
}
