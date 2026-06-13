/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * cap_audio_stream — PCM-over-UDP audio TX and RX as standalone capabilities.
 *
 * Registers four capabilities:
 *
 *   audio_stream_tx_start   Open DMIC; while PTT button held, stream raw PCM to peer via UDP.
 *   audio_stream_rx_start   Receive raw PCM from peer via UDP and play on speaker.
 *   audio_stream_stop       Signal both TX and RX tasks to exit.
 *   audio_stream_status     Query running state and packet counters.
 *
 * SPORT0 full-duplex rules (enforced internally):
 *   1. audio_sp_open (speaker/SPORT0-TX) must be called before audio_dmic_open (DMIC/SPORT0-RX).
 *   2. TX and RX must share the same sample_rate.
 *   3. The receiver_task polls s_ctx->dmic_ready and opens the speaker only after DMIC is up.
 *
 * Calling order for the LLM:
 *   audio_stream_rx_start  →  audio_stream_tx_start
 * Both are non-blocking (tasks run in the background).
 * The internal dmic_ready flag handles synchronisation between the two tasks.
 */

#include "cap_audio_stream.h"
#include "claw_cap.h"
#include <cJSON.h>
#include "ameba_soc.h"
#include "os_wrapper.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lua_driver_audio.h"
#include <string.h>
#include <stdlib.h>

#define TAG             "cap_as"
#define AS_TASK_STACK   4096
#define AS_TASK_PRIO    3
#define AS_RX_BUF_SIZE  4096   /* = AUDIO_CHUNK_SIZE — one DMA chunk */
#define AS_RX_TIMEOUT   200    /* ms — write silence after this */

/* Default hardware pins for RTL8721F eval board */
#define AS_DEFAULT_SR           16000
#define AS_DEFAULT_DMIC_CLK     35u   /* PB3 = 0x23 */
#define AS_DEFAULT_DMIC_DATA    36u   /* PB4 = 0x24 */
#define AS_DEFAULT_ACTIVE_LOW   1

/* ---- pin name parser: "PA_15" / "A_15" → GPIO PinName integer ------------ */

static int parse_pin(const char *s, u32 *out)
{
    if (!s) return -1;
    const char *p = s;
    if (p[0] == 'P') p++;
    if (!p[0] || p[1] != '_') return -1;
    int port = p[0] - 'A';
    if (port < 0 || port > 7) return -1;
    char *endp;
    long num = strtol(p + 2, &endp, 10);
    if (endp == p + 2 || *endp != '\0' || num < 0 || num > 31) return -1;
    *out = (u32)(port * 32 + num);
    return 0;
}

/* ---- shared context ------------------------------------------------------- */

typedef struct {
    /* TX config */
    char     peer_ip[64];
    uint16_t peer_port;
    u32      gpio_pin;
    int      gpio_active_low;
    uint32_t dmic_clk_pin;
    uint32_t dmic_data_pin;

    /* RX config */
    uint16_t local_port;

    /* Shared */
    uint32_t sample_rate;
    volatile int  stop_requested;
    volatile int  dmic_ready;   /* 0=pending, +1=ready, -1=DMIC open failed */
    volatile int  tx_running;   /* 1 while tx_task alive */
    volatile int  rx_running;   /* 1 while receiver_task alive */
    volatile uint32_t packets_sent;
    volatile uint32_t packets_recv;
    rtos_task_t   tx_task;
    rtos_task_t   rx_task;
} as_ctx_t;

static as_ctx_t *s_ctx;

/* ---- TX task: DMIC → UDP with PTT gate ------------------------------------ */

static void tx_task(void *arg)
{
    as_ctx_t *ctx = (as_ctx_t *)arg;

    /* Open DMIC — this is SPORT0 RX; must happen before speaker (SPORT0 TX)
     * if no speaker was previously opened.  The receiver_task waits on
     * ctx->dmic_ready before opening the speaker, so the order is always:
     *   audio_dmic_open → (dmic_ready=1) → audio_sp_open. */
    if (audio_dmic_open(ctx->sample_rate, 1,
                        ctx->dmic_clk_pin, ctx->dmic_data_pin) != 0) {
        RTK_LOGE(TAG, "DMIC open failed\n");
        ctx->dmic_ready = -1;
        ctx->tx_running = 0;
        rtos_task_delete(NULL);
        return;
    }
    ctx->dmic_ready = 1;

    /* PTT GPIO — configure as input with pull-up */
    GPIO_InitTypeDef gpio_cfg;
    gpio_cfg.GPIO_Pin  = ctx->gpio_pin;
    gpio_cfg.GPIO_Mode = GPIO_Mode_IN;
    gpio_cfg.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(&gpio_cfg);

    int sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        RTK_LOGE(TAG, "TX socket failed\n");
        audio_dmic_close();
        ctx->tx_running = 0;
        rtos_task_delete(NULL);
        return;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(ctx->peer_port);
    if (!inet_aton(ctx->peer_ip, &dst.sin_addr)) {
        RTK_LOGE(TAG, "TX invalid peer_ip: %s\n", ctx->peer_ip);
        lwip_close(sock);
        audio_dmic_close();
        ctx->tx_running = 0;
        rtos_task_delete(NULL);
        return;
    }

    RTK_LOGI(TAG, "TX ready — peer=%s:%u gpio=%lu\n",
             ctx->peer_ip, (unsigned)ctx->peer_port, (unsigned long)ctx->gpio_pin);

    /* Heap-allocate TX copy buffer: audio_dmic_read_chunk returns a pointer
     * into the live DMA ping-pong buffer — copy before sendto to prevent the
     * DMA ISR from switching buffers mid-send. */
    uint8_t *copy_buf = (uint8_t *)malloc(AS_RX_BUF_SIZE);
    if (!copy_buf) {
        RTK_LOGE(TAG, "TX copy buf alloc failed\n");
        lwip_close(sock);
        audio_dmic_close();
        ctx->tx_running = 0;
        rtos_task_delete(NULL);
        return;
    }

    while (!ctx->stop_requested) {
        /* Always drain DMIC DMA to keep buffer fresh */
        const uint8_t *chunk = NULL;
        int n = audio_dmic_read_chunk(&chunk, 2000);

        u32 pin_val = GPIO_ReadDataBit(ctx->gpio_pin);
        int pressed = ctx->gpio_active_low ? ((int)pin_val == 0) : ((int)pin_val != 0);

        if (pressed && n > 0 && chunk) {
            memcpy(copy_buf, chunk, (size_t)n);
            lwip_sendto(sock, copy_buf, (size_t)n, 0,
                        (struct sockaddr *)&dst, sizeof(dst));
            ctx->packets_sent++;
        }
    }

    free(copy_buf);
    lwip_close(sock);
    audio_dmic_close();
    ctx->tx_running = 0;
    rtos_task_delete(NULL);
}

/* ---- RX task: UDP → Speaker with silence on timeout ----------------------- */

static void rx_task(void *arg)
{
    as_ctx_t *ctx = (as_ctx_t *)arg;

    /* Wait for DMIC to open (set by tx_task) — up to 3 s.
     * SPORT0 full-duplex rule: speaker (SPORT0-TX) must open after DMIC (SPORT0-RX)
     * to avoid a competing AUDIO_SP_Reset tearing down the DMIC configuration. */
    for (int i = 0; i < 100 && ctx->dmic_ready == 0; i++)
        rtos_time_delay_ms(30);

    if (ctx->dmic_ready < 0) {
        RTK_LOGE(TAG, "RX: DMIC open failed, aborting speaker\n");
        ctx->rx_running = 0;
        rtos_task_delete(NULL);
        return;
    }

    int sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        RTK_LOGE(TAG, "RX socket failed\n");
        ctx->rx_running = 0;
        rtos_task_delete(NULL);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(ctx->local_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        RTK_LOGE(TAG, "RX bind port %u failed\n", (unsigned)ctx->local_port);
        lwip_close(sock);
        ctx->rx_running = 0;
        rtos_task_delete(NULL);
        return;
    }

    /* Timeout > one DMA chunk (128 ms @ 16kHz mono) so "no packet" is detected
     * quickly and silence is fed to the DMA instead of looping the last frame. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = AS_RX_TIMEOUT * 1000};
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t *rx_buf = (uint8_t *)malloc(AS_RX_BUF_SIZE);
    if (!rx_buf) {
        RTK_LOGE(TAG, "RX buf alloc failed\n");
        lwip_close(sock);
        ctx->rx_running = 0;
        rtos_task_delete(NULL);
        return;
    }
    memset(rx_buf, 0, AS_RX_BUF_SIZE);

    if (audio_sp_open(ctx->sample_rate, 1) != 0) {
        RTK_LOGE(TAG, "Speaker open failed\n");
        free(rx_buf);
        lwip_close(sock);
        ctx->stop_requested = 1;  /* tell TX task to stop too */
        ctx->rx_running = 0;
        rtos_task_delete(NULL);
        return;
    }

    RTK_LOGI(TAG, "RX listening on port %u\n", (unsigned)ctx->local_port);

    while (!ctx->stop_requested) {
        int n = lwip_recv(sock, rx_buf, AS_RX_BUF_SIZE, 0);
        if (n > 0) {
            ctx->packets_recv++;
            /* write_chunk zero-fills remainder — no stale audio leaks */
            audio_sp_write_chunk(rx_buf, (uint32_t)n);
        } else {
            /* Timeout — write silence to prevent DMA looping last frame */
            memset(rx_buf, 0, AS_RX_BUF_SIZE);
            audio_sp_write_chunk(rx_buf, AS_RX_BUF_SIZE);
        }
    }

    audio_sp_close();
    free(rx_buf);
    lwip_close(sock);
    ctx->rx_running = 0;
    rtos_task_delete(NULL);
}

/* ---- helpers -------------------------------------------------------------- */

static as_ctx_t *get_or_alloc_ctx(void)
{
    /* Lazily free a fully-stopped context from a previous run */
    if (s_ctx && !s_ctx->tx_running && !s_ctx->rx_running &&
        s_ctx->stop_requested) {
        free(s_ctx);
        s_ctx = NULL;
    }
    if (!s_ctx) {
        s_ctx = (as_ctx_t *)malloc(sizeof(as_ctx_t));
        if (s_ctx) memset(s_ctx, 0, sizeof(*s_ctx));
    }
    return s_ctx;
}

/* ---- capability: audio_stream_tx_start ------------------------------------ */

static int execute_tx_start(const char *input_json,
                             const claw_cap_call_context_t *ctx_arg,
                             char **output)
{
    (void)ctx_arg;

    if (s_ctx && s_ctx->tx_running)
        return claw_cap_set_output(output,
            "{\"ok\":false,\"error\":\"TX already running, call audio_stream_stop first\"}");

    cJSON *j = cJSON_Parse(input_json ? input_json : "{}");
    if (!j)
        return claw_cap_set_output(output, "{\"ok\":false,\"error\":\"invalid json\"}");

    const char *ip_str  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "peer_ip"));
    cJSON      *jp_port = cJSON_GetObjectItem(j, "port");
    const char *pin_str = cJSON_GetStringValue(cJSON_GetObjectItem(j, "gpio_pin"));

    if (!ip_str || !jp_port || !pin_str) {
        cJSON_Delete(j);
        return claw_cap_set_output(output,
            "{\"ok\":false,\"error\":\"required: peer_ip, port, gpio_pin\"}");
    }

    u32 gpio_pin;
    if (parse_pin(pin_str, &gpio_pin) != 0) {
        cJSON_Delete(j);
        return claw_cap_set_output(output,
            "{\"ok\":false,\"error\":\"invalid gpio_pin, e.g. PA_15\"}");
    }

    as_ctx_t *c = get_or_alloc_ctx();
    if (!c) {
        cJSON_Delete(j);
        return claw_cap_set_output(output, "{\"ok\":false,\"error\":\"oom\"}");
    }

    DiagSnPrintf(c->peer_ip, sizeof(c->peer_ip), "%s", ip_str);
    c->peer_port      = (uint16_t)cJSON_GetNumberValue(jp_port);
    c->gpio_pin       = gpio_pin;

    cJSON *ja = cJSON_GetObjectItem(j, "gpio_active_low");
    c->gpio_active_low = ja ? (cJSON_IsTrue(ja) ? 1 : 0) : AS_DEFAULT_ACTIVE_LOW;

    cJSON *jsr = cJSON_GetObjectItem(j, "sample_rate");
    c->sample_rate     = jsr ? (uint32_t)cJSON_GetNumberValue(jsr) : AS_DEFAULT_SR;

    cJSON *jclk = cJSON_GetObjectItem(j, "dmic_clk_pin");
    c->dmic_clk_pin    = jclk ? (uint32_t)cJSON_GetNumberValue(jclk) : AS_DEFAULT_DMIC_CLK;

    cJSON *jdata = cJSON_GetObjectItem(j, "dmic_data_pin");
    c->dmic_data_pin   = jdata ? (uint32_t)cJSON_GetNumberValue(jdata) : AS_DEFAULT_DMIC_DATA;

    /* If RX task is already running it owns stop_requested; reset only if both idle */
    if (!c->rx_running) {
        c->stop_requested = 0;
        c->dmic_ready     = 0;
        c->packets_sent   = 0;
    }

    char ip_buf[64];
    uint16_t port = c->peer_port;
    DiagSnPrintf(ip_buf, sizeof(ip_buf), "%s", ip_str);
    cJSON_Delete(j);

    c->tx_running = 1;
    if (rtos_task_create(&c->tx_task, "as_tx", tx_task, c,
                         AS_TASK_STACK, AS_TASK_PRIO) != RTK_SUCCESS) {
        c->tx_running = 0;
        return claw_cap_set_output(output, "{\"ok\":false,\"error\":\"task create failed\"}");
    }

    return claw_cap_set_output(output,
        "{\"ok\":true,\"peer_ip\":\"%s\",\"port\":%u,"
        "\"msg\":\"TX started — hold PTT button to transmit\"}",
        ip_buf, (unsigned)port);
}

/* ---- capability: audio_stream_rx_start ------------------------------------ */

static int execute_rx_start(const char *input_json,
                             const claw_cap_call_context_t *ctx_arg,
                             char **output)
{
    (void)ctx_arg;

    if (s_ctx && s_ctx->rx_running)
        return claw_cap_set_output(output,
            "{\"ok\":false,\"error\":\"RX already running, call audio_stream_stop first\"}");

    cJSON *j = cJSON_Parse(input_json ? input_json : "{}");
    if (!j)
        return claw_cap_set_output(output, "{\"ok\":false,\"error\":\"invalid json\"}");

    cJSON *jp    = cJSON_GetObjectItem(j, "port");
    cJSON *jsr   = cJSON_GetObjectItem(j, "sample_rate");

    if (!jp) {
        cJSON_Delete(j);
        return claw_cap_set_output(output,
            "{\"ok\":false,\"error\":\"required: port\"}");
    }

    as_ctx_t *c = get_or_alloc_ctx();
    if (!c) {
        cJSON_Delete(j);
        return claw_cap_set_output(output, "{\"ok\":false,\"error\":\"oom\"}");
    }

    c->local_port  = (uint16_t)cJSON_GetNumberValue(jp);
    c->sample_rate = jsr ? (uint32_t)cJSON_GetNumberValue(jsr) : AS_DEFAULT_SR;

    if (!c->tx_running) {
        /* TX not started yet — reset dmic_ready so rx_task will wait */
        c->dmic_ready = 0;
        c->stop_requested = 0;
        c->packets_recv = 0;
    }

    uint16_t port = c->local_port;
    cJSON_Delete(j);

    c->rx_running = 1;
    if (rtos_task_create(&c->rx_task, "as_rx", rx_task, c,
                         AS_TASK_STACK, AS_TASK_PRIO) != RTK_SUCCESS) {
        c->rx_running = 0;
        return claw_cap_set_output(output, "{\"ok\":false,\"error\":\"task create failed\"}");
    }

    return claw_cap_set_output(output,
        "{\"ok\":true,\"port\":%u,"
        "\"msg\":\"RX started — playing peer audio on speaker\"}",
        (unsigned)port);
}

/* ---- capability: audio_stream_stop ---------------------------------------- */

/* Maximum time to wait for TX and RX tasks to exit after stop is requested.
 * Must be long enough for the tasks to complete their current DMA/socket
 * operation.  Both tasks poll stop_requested inside loops with at most
 * AS_RX_TIMEOUT (200 ms) blocking, so 2 s is a generous upper bound.
 *
 * WHY we wait: execute_stop returns to the engine task, which immediately
 * makes the next LLM HTTP request (TCP).  If the RX/TX UDP sockets are still
 * open at that point, two concurrent lwip socket operations hit the WiFi
 * TrustZone NP firmware and trigger a BusFault (R3=0xdeadbeef @ 0x00112e26).
 * Waiting here ensures both sockets are closed before we return. */
#define AS_STOP_WAIT_MS  2000

static int execute_stop(const char *input_json,
                        const claw_cap_call_context_t *ctx_arg,
                        char **output)
{
    (void)input_json; (void)ctx_arg;

    if (!s_ctx || (!s_ctx->tx_running && !s_ctx->rx_running))
        return claw_cap_set_output(output, "{\"ok\":true,\"msg\":\"not running\"}");

    uint32_t sent = s_ctx->packets_sent;
    uint32_t recv = s_ctx->packets_recv;
    s_ctx->stop_requested = 1;

    /* Wait for both tasks to exit so their UDP sockets are closed before
     * this function returns to the engine task. */
    uint32_t waited = 0;
    while (waited < AS_STOP_WAIT_MS) {
        if (!s_ctx->tx_running && !s_ctx->rx_running)
            break;
        rtos_time_delay_ms(50);
        waited += 50;
    }
    if (s_ctx->tx_running || s_ctx->rx_running) {
        RTK_LOGW(TAG, "stop: tasks still running after %u ms\n",
                 (unsigned)AS_STOP_WAIT_MS);
    }

    return claw_cap_set_output(output,
        "{\"ok\":true,\"status\":\"stopped\","
        "\"packets_sent\":%lu,\"packets_recv\":%lu}",
        (unsigned long)sent, (unsigned long)recv);
}

/* ---- capability: audio_stream_status -------------------------------------- */

static int execute_status(const char *input_json,
                          const claw_cap_call_context_t *ctx_arg,
                          char **output)
{
    (void)input_json; (void)ctx_arg;

    if (!s_ctx)
        return claw_cap_set_output(output, "{\"tx\":\"idle\",\"rx\":\"idle\"}");

    return claw_cap_set_output(output,
        "{\"tx\":\"%s\",\"rx\":\"%s\","
        "\"packets_sent\":%lu,\"packets_recv\":%lu,"
        "\"peer_ip\":\"%s\",\"port\":%u,\"sample_rate\":%lu}",
        s_ctx->tx_running ? "running" : "stopped",
        s_ctx->rx_running ? "running" : "stopped",
        (unsigned long)s_ctx->packets_sent,
        (unsigned long)s_ctx->packets_recv,
        s_ctx->peer_ip,
        (unsigned)s_ctx->peer_port,
        (unsigned long)s_ctx->sample_rate);
}

/* ---- capability registration ---------------------------------------------- */

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "audio_stream_tx_start",
        .name        = "audio_stream_tx_start",
        .family      = "audio",
        .description =
            "Start DMIC→UDP audio streaming to a peer. Call this ONCE — the C-layer "
            "background task automatically reads gpio_pin every loop and only sends "
            "audio while the button is held (active-low by default). "
            "Do NOT poll gpio_pin in Lua or call this repeatedly. "
            "NEVER reimplement audio streaming in Lua. "
            "Call AFTER audio_stream_rx_start. "
            "Required: peer_ip (from net_discover_peer), port (e.g. 9000), "
            "gpio_pin (PTT button pin, e.g. PA_15 — C layer manages it automatically).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"peer_ip\":{\"type\":\"string\","
                         "\"description\":\"Peer IP address (from net_discover_peer)\"},"
            "\"port\":{\"type\":\"integer\","
                      "\"description\":\"UDP port to send audio to (e.g. 9000)\"},"
            "\"gpio_pin\":{\"type\":\"string\","
                          "\"description\":\"PTT button pin (e.g. PA_15). "
                          "The C task polls this pin automatically — do NOT read it in Lua.\"},"
            "\"gpio_active_low\":{\"type\":\"boolean\","
                                 "\"description\":\"true if pressing pulls pin low (default true)\"},"
            "\"sample_rate\":{\"type\":\"integer\","
                             "\"description\":\"PCM sample rate Hz (default 16000)\"},"
            "\"dmic_clk_pin\":{\"type\":\"integer\","
                              "\"description\":\"DMIC clock pinmux index (default 35 = PB3)\"},"
            "\"dmic_data_pin\":{\"type\":\"integer\","
                               "\"description\":\"DMIC data pinmux index (default 36 = PB4)\"}"
            "},"
            "\"required\":[\"peer_ip\",\"port\",\"gpio_pin\"]}",
        .execute     = execute_tx_start,
    },
    {
        .id          = "audio_stream_rx_start",
        .name        = "audio_stream_rx_start",
        .family      = "audio",
        .description =
            "Listen on a UDP port for raw PCM audio from the peer and play it on "
            "the speaker (I2S MAX98357A). Runs as a C-layer background task — "
            "NEVER reimplement audio reception in Lua. "
            "Call BEFORE audio_stream_tx_start. "
            "Required: port (e.g. 9000).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"port\":{\"type\":\"integer\","
                      "\"description\":\"Local UDP port to receive audio on (e.g. 9000)\"},"
            "\"sample_rate\":{\"type\":\"integer\","
                             "\"description\":\"PCM sample rate Hz (default 16000)\"}"
            "},"
            "\"required\":[\"port\"]}",
        .execute     = execute_rx_start,
    },
    {
        .id          = "audio_stream_stop",
        .name        = "audio_stream_stop",
        .family      = "audio",
        .description = "Stop both TX (DMIC→UDP) and RX (UDP→Speaker) stream tasks.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_stop,
    },
    {
        .id          = "audio_stream_status",
        .name        = "audio_stream_status",
        .family      = "audio",
        .description = "Get TX/RX stream state and UDP packet counters.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_status,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "audio_stream",
    .plugin_name      = "cap_audio_stream",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 4,
};

int cap_audio_stream_init(void)
{
    return claw_cap_register_group(&s_group);
}
