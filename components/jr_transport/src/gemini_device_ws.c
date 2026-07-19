/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_transport/gemini_device_ws.c — esp_websocket_client impl of the lower
 * ws_transport seam. This is the ONLY file in the transport that references
 * esp_websocket_client; the host-tested framer/parser (gemini_live.c) sit ABOVE
 * it and are already green. The load-bearing translation it owns: a benign TCP
 * would-block (poll_write==0) maps to JR_WS_TX_WOULD_BLOCK (backpressure) —
 * NEVER a teardown (v4's worst bug: a transient stall aborted the WSS).
 *
 * Config harvested from v4 gl (spec §ws-client): host/path split with the API
 * key as a query param, WEBSOCKET_TRANSPORT_OVER_SSL + crt_bundle, keepalive
 * ping trio, disable_auto_reconnect (the L3 machine owns reconnection). The
 * 16 KB SND_BUF/WND that absorbs RTT spikes is a global lwIP sdkconfig knob
 * (sdkconfig.defaults), not a per-socket setsockopt.
 */
#include "jr_transport/gemini_device_ws.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "jr_transport/gemini_live.h"

static const char *TAG = "jr_ws";

/* Bound one inbound WS message; a 24 kHz base64 audio chunk is the large case.
 * Frames whose total payload exceeds this are dropped (bounded memory). */
#define JR_WS_DEV_RX_MAX     JR_GEMINI_RX_MAX
#define JR_WS_DEV_RING_DEPTH 24
/* Send timeout: short enough to never stall the pump, long enough that a small
 * RTT spike returns 0 (would-block, buffered) instead of a partial write. */
/* Lock-acquire patience for a WS send. Raised 30->60: under server-VAD the tx
 * (mic uplink) contends with heavy rx audio for the single ws-client lock;
 * 30 ms (3 ticks) lost the race constantly and floods "Could not lock
 * ws-client". 60 ms still bounds the voice-loop stall below the 128 ms batch. */
#define JR_WS_DEV_SEND_TO_MS 60

/* A reassembled inbound frame handed to recv_frame() (heap, PSRAM-first). */
typedef struct {
    size_t len;
    char  *data;   /* NUL-terminated, len bytes + '\0' */
} jr_ws_frame_t;

static struct {
    esp_websocket_client_handle_t client;
    QueueHandle_t                 rx_ring;      /* of jr_ws_frame_t             */
    char                          url[512];
    volatile jr_ws_state_t        state;
    volatile uint64_t             last_rx_ms;

    /* fragment reassembly (WEBSOCKET_EVENT_DATA may deliver a payload in parts) */
    char   *acc;
    size_t  acc_cap;
    size_t  acc_have;
    bool    acc_drop;   /* current oversize frame: swallow the rest             */
    bool    inited;
} s_ws;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void push_frame(const char *data, size_t len)
{
    if (s_ws.rx_ring == NULL || len == 0) {
        return;
    }
    jr_ws_frame_t f;
    f.len = len;
    f.data = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (f.data == NULL) {
        f.data = (char *)malloc(len + 1);   /* PSRAM starved -> internal */
    }
    if (f.data == NULL) {
        return;                              /* drop under OOM, never crash */
    }
    memcpy(f.data, data, len);
    f.data[len] = '\0';
    if (xQueueSend(s_ws.rx_ring, &f, 0) != pdTRUE) {
        free(f.data);                        /* ring full -> drop-oldest-less: drop */
    }
}

/* Reassemble one WEBSOCKET_EVENT_DATA chunk; push a complete frame to the ring. */
static void on_data(const esp_websocket_event_data_t *ev)
{
    int total = ev->payload_len;
    int off   = ev->payload_offset;
    int dlen  = ev->data_len;
    if (total <= 0 || dlen < 0) {
        return;
    }

    /* Fast path: a whole small frame arrives in one event. */
    if (off == 0 && dlen >= total) {
        if (total < JR_WS_DEV_RX_MAX) {
            push_frame(ev->data_ptr, (size_t)total);
        } else {
            ESP_LOGW(TAG, "dropping oversize frame (%d bytes)", total);
        }
        return;
    }

    /* Fragmented: accumulate into a heap buffer sized to the total payload. */
    if (off == 0) {
        s_ws.acc_have = 0;
        s_ws.acc_drop = (total >= JR_WS_DEV_RX_MAX);
        if (!s_ws.acc_drop) {
            if (s_ws.acc_cap < (size_t)total + 1) {
                char *n = (char *)heap_caps_realloc(s_ws.acc, (size_t)total + 1,
                                                    MALLOC_CAP_SPIRAM);
                if (n == NULL) {
                    n = (char *)realloc(s_ws.acc, (size_t)total + 1);
                }
                if (n == NULL) {
                    s_ws.acc_drop = true;   /* OOM: swallow this frame */
                } else {
                    s_ws.acc = n;
                    s_ws.acc_cap = (size_t)total + 1;
                }
            }
        }
        if (s_ws.acc_drop) {
            ESP_LOGW(TAG, "dropping oversize/OOM fragmented frame (%d bytes)", total);
        }
    }
    if (!s_ws.acc_drop && s_ws.acc != NULL &&
        (size_t)off + (size_t)dlen <= s_ws.acc_cap) {
        memcpy(s_ws.acc + off, ev->data_ptr, (size_t)dlen);
        s_ws.acc_have = (size_t)off + (size_t)dlen;
    }
    if (off + dlen >= total) {   /* last fragment */
        if (!s_ws.acc_drop && s_ws.acc != NULL && s_ws.acc_have == (size_t)total) {
            push_frame(s_ws.acc, s_ws.acc_have);
        }
        s_ws.acc_have = 0;
        s_ws.acc_drop = false;
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws.state = JR_WS_OPEN;
        s_ws.last_rx_ms = now_ms();
        ESP_LOGI(TAG, "ws connected");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (ev != NULL) {
            s_ws.last_rx_ms = now_ms();
        }
        if (ev != NULL && (ev->op_code == 0x1 /*TEXT*/ || ev->op_code == 0x2 /*BIN*/)) {
            on_data(ev);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_ws.state = JR_WS_CLOSED;
        ESP_LOGW(TAG, "ws disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        s_ws.state = JR_WS_ERROR;
        ESP_LOGE(TAG, "ws error");
        break;
    default:
        break;
    }
}

/* ------------------------------ port impl ------------------------------- */

static jr_err_t dev_connect(void *ctx, const char *url)
{
    (void)ctx;
    const char *use = (url != NULL && url[0] != '\0') ? url : s_ws.url;
    if (use == NULL || use[0] == '\0') {
        ESP_LOGE(TAG, "connect: no URL");
        return JR_ERR_INVALID;
    }

    /* Reuse the handle across L3 reconnects: stop old, re-init fresh config. */
    if (s_ws.client != NULL) {
        esp_websocket_client_stop(s_ws.client);
        esp_websocket_client_destroy(s_ws.client);
        s_ws.client = NULL;
    }

    esp_websocket_client_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.uri                    = use;   /* full wss URL incl. ?key= (overrides host/path) */
    cfg.transport              = WEBSOCKET_TRANSPORT_OVER_SSL;
    cfg.buffer_size            = 4096;
    /* Measured peak 3,596 B over full voice turns; 6144 leaves 2,548 B margin.
     * Was 8192. Internal RAM is the binding resource — see the budget table in
     * docs/JARVISNANO_OS_PLAN.md. Re-measure /api/diag/tasks after any change to
     * frame sizes or buffer_size above. */
    cfg.task_stack             = 6144;
    cfg.task_prio              = 5;
    cfg.network_timeout_ms     = 10000;
    cfg.reconnect_timeout_ms   = 5000;
    cfg.disable_auto_reconnect = true;   /* the L3 machine owns reconnection */
    cfg.ping_interval_sec      = 20;
    cfg.pingpong_timeout_sec   = 20;
    cfg.keep_alive_enable      = true;
    cfg.crt_bundle_attach      = esp_crt_bundle_attach;

    s_ws.client = esp_websocket_client_init(&cfg);
    if (s_ws.client == NULL) {
        s_ws.state = JR_WS_ERROR;
        return JR_ERR_NO_MEM;
    }
    esp_websocket_register_events(s_ws.client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);
    s_ws.state = JR_WS_CONNECTING;
    esp_err_t e = esp_websocket_client_start(s_ws.client);
    if (e != ESP_OK) {
        s_ws.state = JR_WS_ERROR;
        return JR_ERR_FAIL;
    }
    return JR_OK;   /* async: CONNECTED arrives via the event handler */
}

static jr_ws_send_result_t dev_send_bytes(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    jr_ws_send_result_t r = { JR_WS_TX_CLOSED, 0 };
    if (s_ws.client == NULL) {
        return r;
    }
    int sent = esp_websocket_client_send_text(s_ws.client, (const char *)buf,
                                              (int)len, pdMS_TO_TICKS(JR_WS_DEV_SEND_TO_MS));
    if (sent == (int)len) {
        r.kind = JR_WS_TX_OK;      r.sent = (size_t)sent;
    } else if (sent > 0) {
        r.kind = JR_WS_TX_PARTIAL; r.sent = (size_t)sent;
    } else if (sent == 0) {
        /* poll_write timed out with the socket still up: BACKPRESSURE, not a
         * teardown. THE fix — never abort here. */
        r.kind = JR_WS_TX_WOULD_BLOCK;
    } else if (esp_websocket_client_is_connected(s_ws.client)) {
        r.kind = JR_WS_TX_SOFT_FAIL;   /* error but socket open -> dead-uplink count */
    } else {
        r.kind = JR_WS_TX_CLOSED;      /* the death path */
    }
    return r;
}

static int dev_recv_frame(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    if (s_ws.rx_ring == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    jr_ws_frame_t f;
    if (xQueueReceive(s_ws.rx_ring, &f, 0) != pdTRUE) {
        return 0;   /* nothing available now */
    }
    size_t n = (f.len < cap - 1) ? f.len : (cap - 1);
    memcpy(buf, f.data, n);
    buf[n] = '\0';
    free(f.data);
    s_ws.last_rx_ms = now_ms();
    return (int)n;
}

static jr_ws_state_t dev_state(void *ctx)
{
    (void)ctx;
    return s_ws.client ? s_ws.state : JR_WS_CLOSED;
}

static uint64_t dev_last_rx_ms(void *ctx)
{
    (void)ctx;
    return s_ws.last_rx_ms;
}

static void dev_close(void *ctx)
{
    (void)ctx;
    if (s_ws.client != NULL) {
        esp_websocket_client_stop(s_ws.client);
        esp_websocket_client_destroy(s_ws.client);
        s_ws.client = NULL;
    }
    s_ws.state = JR_WS_CLOSED;
    /* drain any queued frames so a fresh session starts clean */
    if (s_ws.rx_ring != NULL) {
        jr_ws_frame_t f;
        while (xQueueReceive(s_ws.rx_ring, &f, 0) == pdTRUE) {
            free(f.data);
        }
    }
}

esp_err_t jr_gemini_ws_init(const char *url)
{
    if (!s_ws.inited) {
        s_ws.rx_ring = xQueueCreate(JR_WS_DEV_RING_DEPTH, sizeof(jr_ws_frame_t));
        if (s_ws.rx_ring == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_ws.state = JR_WS_CLOSED;
        s_ws.inited = true;
    }
    if (url != NULL) {
        strlcpy(s_ws.url, url, sizeof s_ws.url);
    }
    return ESP_OK;
}

jr_ws_transport_t jr_gemini_ws(void)
{
    jr_ws_transport_t t;
    t.ctx        = &s_ws;
    t.connect    = dev_connect;
    t.send_bytes = dev_send_bytes;
    t.recv_frame = dev_recv_frame;
    t.state      = dev_state;
    t.last_rx_ms = dev_last_rx_ms;
    t.close      = dev_close;
    return t;
}
