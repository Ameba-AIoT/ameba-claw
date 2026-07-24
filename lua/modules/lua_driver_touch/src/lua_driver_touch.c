/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** lua_driver_touch.c — capacitive touch-panel driver (GT911 today, IC-agnostic
** engine).  require("touch") for the st7701p 480x480 panel's touch IC.
**
**   touch.init(id)     — read pins from board.json device `id`, select the
**                        per-chip ops by its `chip` field, bring up I2C0 + the
**                        INT ISR + a reader task.  Returns true | nil,errmsg.
**   touch.deinit()     — stop reader task, release INT ISR + I2C.
**   touch.get_event()  — NON-BLOCKING: pop one raw event {type,x,y,dx,dy} from
**                        the queue, or nil.  type = "down"|"move"|"up".
**   touch.width()/height() — configured panel resolution (post-init).
**
** ── Architecture (design_spec/display/phase4_touch_gt911.md) ─────────────────
** Two layers:
**   1. A tiny per-chip ops table (touch_chip_ops_t): reset / probe / read_points.
**      Only these three functions are IC-specific; adding a new touch IC (e.g.
**      CST328) means adding one s_chips[] entry — same pattern as the display
**      LCDC per-chip panel table.
**   2. A generic engine: an INT ISR that only gives a semaphore, a reader TASK
**      that does all I2C (illegal in ISR) and runs the down/move/up FSM, and an
**      event QUEUE the task feeds.  get_event() just drains the queue.
**
** Capturing events in the reader task (woken on every INT, ~5-10 ms while
** touched) instead of inline in get_event() decouples capture from the Lua poll
** rate: a fast tap that begins and ends between two get_event() calls is still
** delivered.  Gesture recognition (tap/swipe/long_press) lives in pure Lua
** (rolfs:/lib/gesture.lua) on top of this raw stream.
**
** GT911 bring-up (reset timing / address-select polarity / I2C IC_FILTER noise
** patch / register map) is lifted verbatim from the board-proven SDK driver
** component/ui/input/input_touch_gt911.c — do NOT re-derive it.
*/
#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lua_driver_touch.h"
#include "ameba_soc.h"
#include "os_wrapper.h"
#include "ameba_claw_defs.h"

#include "i2c_api.h"
#include "i2c_ex_api.h"
#include "gpio_api.h"
#include "gpio_irq_api.h"

#include "claw_cap.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

#define TOUCH_LOG "GT911"

/* ── Raw event queue element ──────────────────────────────────────────────── */
enum { TOUCH_EV_DOWN = 0, TOUCH_EV_MOVE, TOUCH_EV_UP };
static const char *const s_ev_names[] = { "down", "move", "up" };

typedef struct {
    uint8_t type;
    int16_t x, y, dx, dy;
} touch_event_t;

/* ── One decoded contact point ────────────────────────────────────────────── */
typedef struct {
    int x, y;   /* panel-oriented pixels */
} touch_pt_t;

/* ── Per-panel context (single panel) — passed to the chip ops ────────────── */
typedef struct touch_ctx {
    i2c_t    i2c;
    uint16_t rst_pin, int_pin, sda_pin, scl_pin;
    uint8_t  addr;              /* 7-bit I2C address (0x14) */
    int      width, height;
    uint8_t  mirror_x;          /* invert X axis (board.json params.mirror_x, default 1) */
    uint8_t  mirror_y;          /* invert Y axis (board.json params.mirror_y, default 1) */
} touch_ctx_t;

/* ── Per-chip ops (the ONLY IC-specific surface) ──────────────────────────── */
typedef struct {
    const char *chip;                                          /* board.json "chip", case-insensitive */
    int (*reset)(touch_ctx_t *);                               /* GPIO reset + address select; 0 ok    */
    int (*probe)(touch_ctx_t *);                               /* verify comms (read id); 0 ok         */
    int (*read_points)(touch_ctx_t *, touch_pt_t *, int max);  /* >=0 count (0=up), <0 = skip          */
} touch_chip_ops_t;

/* ── Driver state ─────────────────────────────────────────────────────────── */
static touch_ctx_t             s_ctx;
static const touch_chip_ops_t *s_ops;
static int                     s_inited;

static gpio_irq_t   s_irq;
static rtos_sema_t  s_sema;        /* INT wakeup (binary; latest state wins)     */
static rtos_sema_t  s_task_done;   /* reader task exit handshake                 */
static rtos_queue_t s_queue;       /* raw events → get_event()                   */
static int          s_queue_consumer; /* 1 = someone drains s_queue this session */
static volatile int s_running;     /* reader task run flag                       */

/* Reader-task-owned FSM state (single task; reset in init).  s_snapshot_lock
 * guards these three fields: the reader task is the sole writer, but once the
 * `lvgl` mode is active lv_indev's read_cb (running on lvgl_timer_task) also
 * reads them via touch_engine_snapshot() — without the lock a torn read could
 * pair a new x with a stale y (see phase5_lvgl_full.md §11). Lua's
 * get_event() does not need it (it only drains s_queue). */
static rtos_mutex_t s_snapshot_lock;
static int s_prev_pressed;
static int s_emit_x, s_emit_y;     /* last EMITTED position (move deltas base)    */

/* ========================================================================== */
/* board.json parsing (mirrors display_backend_lcdc.c)                        */
/* ========================================================================== */

static uint16_t parse_pin(const char *s)
{
    if (!s || (s[0] != 'P' && s[0] != 'p')) {
        return 0xFFFF;
    }
    int port;
    switch (s[1]) {
        case 'A': case 'a': port = 0; break;
        case 'B': case 'b': port = 1; break;
        case 'C': case 'c': port = 2; break;
        default: return 0xFFFF;
    }
    const char *n = (s[2] == '_') ? &s[3] : &s[2];
    char *end;
    long num = strtol(n, &end, 10);
    if (*end != '\0' || num < 0 || num > 31) {
        return 0xFFFF;
    }
    return (uint16_t)((port << 5) | (int)num);
}

static int parse_req_pin(cJSON *params, const char *key, uint16_t *dst,
                         char *err, size_t errlen)
{
    cJSON *p = cJSON_GetObjectItem(params, key);
    if (!p || !cJSON_IsString(p)) {
        snprintf(err, errlen, "touch: params missing pin '%s'", key);
        return -1;
    }
    uint16_t pin = parse_pin(p->valuestring);
    if (pin == 0xFFFF) {
        snprintf(err, errlen, "touch: bad pin '%s'=%s", key, p->valuestring);
        return -1;
    }
    *dst = pin;
    return 0;
}

/* Fetch device `id` from cap_board_mgr; parse wiring/address/resolution into
 * s_ctx and select the chip ops from `chip`.  Returns 0 on success. */
static int load_cfg(const char *id, char *err, size_t errlen);   /* fwd (needs s_chips) */

/* Anchor a __gc sentinel so the engine is released on lua_State teardown even if
 * the script never calls touch.deinit() (defined below; used by l_touch_init). */
static void sentinel_create(lua_State *L);

/* ========================================================================== */
/* GT911 chip ops                                                             */
/* ========================================================================== */

#define GT_PID_REG    0x8140  /* product id (4 bytes)                           */
#define GT_GSTID_REG  0x814E  /* status: bit7=buffer ready, bits[3:0]=count     */
#define GT_TP1_REG    0x8150  /* touch point 1: x_lo,x_hi,y_lo,y_hi,size_lo,hi  */

/* 16-bit big-endian register, write-then-read (NO repeated start). */
static int gt911_i2c_read(touch_ctx_t *c, uint16_t reg, uint8_t *buf, int len)
{
    uint8_t wr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff) };
    if (i2c_write(&c->i2c, c->addr, (const char *)wr, 2, 1) != 2) {
        return -1;
    }
    return i2c_read(&c->i2c, c->addr, (char *)buf, len, 1);
}

static int gt911_i2c_write1(touch_ctx_t *c, uint16_t reg, uint8_t val)
{
    uint8_t wr[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff), val };
    return (i2c_write(&c->i2c, c->addr, (const char *)wr, 3, 1) == 3) ? 0 : -1;
}

/* GT911 reset + address select.  For address 0x14 the INT line must be HIGH at
 * the RST rising edge (proven by SDK board_i2c_init()).  Do NOT re-derive. */
static int gt911_reset(touch_ctx_t *c)
{
    GPIO_InitTypeDef io;

    io.GPIO_Pin  = c->rst_pin;
    io.GPIO_PuPd = GPIO_PuPd_NOPULL;
    io.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_Init(&io);
    io.GPIO_Pin  = c->int_pin;
    io.GPIO_PuPd = GPIO_PuPd_UP;
    io.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_Init(&io);

    GPIO_WriteBit(c->int_pin, 0);
    GPIO_WriteBit(c->rst_pin, 0);
    DelayMs(10);
    GPIO_WriteBit(c->int_pin, 1);   /* INT HIGH before RST rises → select 0x14  */
    DelayUs(100);
    GPIO_WriteBit(c->rst_pin, 1);
    DelayMs(5);                     /* address latch window (INT stays HIGH)    */
    GPIO_WriteBit(c->int_pin, 0);
    DelayMs(50);

    io.GPIO_Pin  = c->int_pin;      /* release INT as input for edge interrupt  */
    io.GPIO_PuPd = GPIO_PuPd_NOPULL;
    io.GPIO_Mode = GPIO_Mode_IN;
    GPIO_Init(&io);
    return 0;
}

static int gt911_probe(touch_ctx_t *c)
{
    uint8_t pid[4] = { 0 };
    if (gt911_i2c_read(c, GT_PID_REG, pid, 4) < 0) {
        return -1;
    }
    RTK_LOGI(TOUCH_LOG, "GT911 id: %c%c%c%c\n", pid[0], pid[1], pid[2], pid[3]);
    return 0;
}

/* Read status; if the ready bit is set, decode point 1 and clear the flag.
 * Returns contact count (0 = finger up), or <0 when the buffer is not ready
 * (nothing to do this wake). */
static int gt911_read_points(touch_ctx_t *c, touch_pt_t *pts, int max)
{
    uint8_t mode = 0;
    if (gt911_i2c_read(c, GT_GSTID_REG, &mode, 1) < 0) {
        return -1;
    }
    if (!(mode & 0x80)) {
        return -1;   /* coordinate buffer not ready — do not clear */
    }

    int n = mode & 0x0F;
    if (n > 0 && max > 0) {
        uint8_t pt[4] = { 0 };   /* x_lo, x_hi, y_lo, y_hi */
        if (gt911_i2c_read(c, GT_TP1_REG, pt, 4) >= 0) {
            int raw_x = pt[0] | (pt[1] << 8);
            int raw_y = pt[2] | (pt[3] << 8);
            /* Axis inversion is panel-specific; configured via board.json mirror_x/mirror_y. */
            int x = c->mirror_x ? (c->width  - raw_x) : raw_x;
            int y = c->mirror_y ? (c->height - raw_y) : raw_y;
            pts[0].x = x < 0 ? 0 : (x > c->width  - 1 ? c->width  - 1 : x);
            pts[0].y = y < 0 ? 0 : (y > c->height - 1 ? c->height - 1 : y);
        } else {
            n = 0;   /* read failed → treat as no valid point */
        }
    }

    /* Must clear the ready flag or the GT911 stops updating / re-INTing. */
    gt911_i2c_write1(c, GT_GSTID_REG, 0);
    return n;
}

/* ── Chip registry — add a new touch IC here (one entry) ──────────────────── */
static const touch_chip_ops_t s_chips[] = {
    { "GT911", gt911_reset, gt911_probe, gt911_read_points },
};

static int load_cfg(const char *id, char *err, size_t errlen)
{
    char  input[96];
    char *out = NULL;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.addr     = 0x14;
    s_ctx.width    = s_ctx.height = 480;
    s_ctx.mirror_x = 1;   /* default: invert (matches legacy st7701p 480x480 behaviour) */
    s_ctx.mirror_y = 1;

    snprintf(input, sizeof(input), "{\"id\":\"%s\"}", id);

    claw_cap_call_context_t ctx = { 0 };
    ctx.caller = CLAW_CAP_CALLER_INTERNAL;
    (void)claw_cap_call("board_get_device", input, &ctx, &out);
    if (!out) {
        snprintf(err, errlen, "touch: board_get_device returned nothing");
        return -1;
    }

    cJSON *root = cJSON_Parse(out);
    free(out);
    if (!root) {
        snprintf(err, errlen, "touch: board_get_device bad JSON");
        return -1;
    }

    cJSON *jerr = cJSON_GetObjectItem(root, "error");
    if (jerr && cJSON_IsString(jerr)) {
        snprintf(err, errlen, "%s", jerr->valuestring);
        cJSON_Delete(root);
        return -1;
    }

    /* Chip → ops (case-insensitive). */
    cJSON *chip = cJSON_GetObjectItem(root, "chip");
    if (!chip || !cJSON_IsString(chip)) {
        snprintf(err, errlen, "touch: device '%s' has no chip", id);
        cJSON_Delete(root);
        return -1;
    }
    s_ops = NULL;
    for (size_t i = 0; i < sizeof(s_chips) / sizeof(s_chips[0]); i++) {
        if (strcasecmp(chip->valuestring, s_chips[i].chip) == 0) {
            s_ops = &s_chips[i];
            break;
        }
    }
    if (!s_ops) {
        snprintf(err, errlen, "touch: unsupported chip '%s'", chip->valuestring);
        cJSON_Delete(root);
        return -1;
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (!params) {
        snprintf(err, errlen, "touch: device '%s' has no params", id);
        cJSON_Delete(root);
        return -1;
    }

    if (parse_req_pin(params, "sda", &s_ctx.sda_pin, err, errlen) ||
        parse_req_pin(params, "scl", &s_ctx.scl_pin, err, errlen) ||
        parse_req_pin(params, "rst", &s_ctx.rst_pin, err, errlen) ||
        parse_req_pin(params, "int", &s_ctx.int_pin, err, errlen)) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *addr = cJSON_GetObjectItem(params, "address");
    if (addr && cJSON_IsString(addr)) {
        s_ctx.addr = (uint8_t)strtol(addr->valuestring, NULL, 0);
    } else if (addr && cJSON_IsNumber(addr)) {
        s_ctx.addr = (uint8_t)addr->valueint;
    }

    cJSON *res = cJSON_GetObjectItem(params, "resolution");
    if (res && cJSON_IsString(res)) {
        int w = 0, h = 0;
        if (sscanf(res->valuestring, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            s_ctx.width  = w;
            s_ctx.height = h;
        }
    }

    cJSON *mx = cJSON_GetObjectItem(params, "mirror_x");
    if (mx && cJSON_IsBool(mx)) {
        s_ctx.mirror_x = cJSON_IsTrue(mx) ? 1 : 0;
    }
    cJSON *my = cJSON_GetObjectItem(params, "mirror_y");
    if (my && cJSON_IsBool(my)) {
        s_ctx.mirror_y = cJSON_IsTrue(my) ? 1 : 0;
    }

    cJSON_Delete(root);
    return 0;
}

/* ========================================================================== */
/* Generic engine: I2C bus bring-up, ISR, reader task                         */
/* ========================================================================== */

static void touch_i2c_bringup(touch_ctx_t *c)
{
    c->i2c.i2c_idx = 0;                 /* I2C0 (idx=0) */
    i2c_init(&c->i2c, c->sda_pin, c->scl_pin);
    i2c_frequency(&c->i2c, 400000);
    i2c_restart_disable(&c->i2c);

    /* PATCH_FOR_LCD_NOISE: i2c_api does not expose IC_FILTER.  RGB-LCD (st7701p)
     * dclk/data toggling couples noise onto the shared I2C lines; without this
     * filter the reads glitch / NAK.  Write the controller register directly
     * (mirrors SDK input_touch_gt911.c). */
    extern I2C_InitTypeDef I2CInitDat[2];
    uint32_t base = (uint32_t)c->i2c.I2Cx;
    uint32_t val;
    I2C_Cmd(c->i2c.I2Cx, DISABLE);
    I2CInitDat[c->i2c.i2c_idx].I2CFilter = CLAW_TOUCH_I2C_FILTER;
    val  = HAL_READ32(base, 0xEC);
    val &= ~0x1FFu;
    val |= CLAW_TOUCH_I2C_FILTER;
    HAL_WRITE32(base, 0xEC, val);

    /* Force the controller target address to c->addr.  i2c_api.c caches the last
     * programmed slave address in a FILE-STATIC (i2c_target_addr[]) and only
     * reprograms IC_TAR when the requested address differs from that cache.  The
     * cache survives across our deinit/init (it is never reset), but i2c_init()
     * POR-resets the I2C controller (RCC DISABLE/ENABLE) and zeroes IC_TAR.  So on
     * the SECOND init the cache still says 0x14 while the hardware TAR is 0 → the
     * first i2c_write sees "0x14 == cached 0x14", skips the reprogram, and talks to
     * address 0 → NAK (TX_ABRT 0x1, "IC not responding").  Writing IC_TAR here (in
     * the disabled window) re-syncs hardware with that stale cache so every re-init
     * addresses the GT911 correctly. */
    I2C_SetSlaveAddress(c->i2c.I2Cx, c->addr);
    I2C_Cmd(c->i2c.I2Cx, ENABLE);
}

/* ISR top-half: only flag + wake.  NEVER touch I2C here (design §6.3). */
static void touch_irq_handler(uint32_t id, uint32_t event)
{
    (void)id;
    (void)event;
    if (s_sema) {
        rtos_sema_give(s_sema);
    }
}

static void touch_enqueue(uint8_t type, int x, int y, int dx, int dy)
{
    if (!s_queue_consumer) {
        /* lvgl mode: nobody calls get_event(), so s_queue would just fill up
         * and warn on every touch forever (see touch_engine_init's want_queue
         * doc comment) — lv_indev reads touch_engine_snapshot() instead. */
        return;
    }
    touch_event_t ev = { type, (int16_t)x, (int16_t)y, (int16_t)dx, (int16_t)dy };
    if (rtos_queue_send(s_queue, &ev, 0) != RTK_SUCCESS && type != TOUCH_EV_MOVE) {
        /* Moves may be dropped when the consumer lags; down/up should not. */
        RTK_LOGW(TOUCH_LOG, "event queue full, dropped %s\n", s_ev_names[type]);
    }
}

/* Update the shared (pressed,x,y) snapshot under s_snapshot_lock — the only
 * writer is this reader task, but touch_engine_snapshot() may read it from
 * lvgl_timer_task concurrently (see the field comment above). */
static void snapshot_set(int pressed, int x, int y)
{
    rtos_mutex_take(s_snapshot_lock, RTOS_MAX_DELAY);
    s_prev_pressed = pressed;
    s_emit_x = x;
    s_emit_y = y;
    rtos_mutex_give(s_snapshot_lock);
}

/* Reader task: woken by the INT ISR, does all I2C, runs the down/move/up FSM.
 * While a contact is active it blocks with a release-timeout so a missed
 * lift-INT can still be turned into an "up" (design §6.9). */
static void touch_reader_task(void *arg)
{
    (void)arg;
    while (s_running) {
        uint32_t wait = s_prev_pressed ? CLAW_TOUCH_RELEASE_TIMEOUT_MS : RTOS_MAX_DELAY;
        int r = rtos_sema_take(s_sema, wait);
        if (!s_running) {
            break;
        }
        if (r != RTK_SUCCESS) {
            /* Release watchdog: no INT for a while while pressed → synthesize up. */
            if (s_prev_pressed) {
                snapshot_set(0, s_emit_x, s_emit_y);
                touch_enqueue(TOUCH_EV_UP, s_emit_x, s_emit_y, 0, 0);
            }
            continue;
        }

        touch_pt_t pt;
        int n = s_ops->read_points(&s_ctx, &pt, 1);
        if (n < 0) {
            continue;   /* buffer not ready / transient glitch */
        }

        if (n > 0) {
            if (!s_prev_pressed) {
                snapshot_set(1, pt.x, pt.y);
                touch_enqueue(TOUCH_EV_DOWN, pt.x, pt.y, 0, 0);
            } else {
                int dx = pt.x - s_emit_x;
                int dy = pt.y - s_emit_y;
                if (dx * dx + dy * dy >=
                    CLAW_TOUCH_MOVE_THRESHOLD * CLAW_TOUCH_MOVE_THRESHOLD) {
                    snapshot_set(1, pt.x, pt.y);
                    touch_enqueue(TOUCH_EV_MOVE, pt.x, pt.y, dx, dy);
                }
            }
        } else {   /* n == 0: finger lifted */
            if (s_prev_pressed) {
                snapshot_set(0, s_emit_x, s_emit_y);
                touch_enqueue(TOUCH_EV_UP, s_emit_x, s_emit_y, 0, 0);
            }
        }
    }

    rtos_sema_give(s_task_done);
    rtos_task_delete(NULL);
}

/* ========================================================================== */
/* Engine init — shared by touch.init(id) (Lua) and lua_module_lvgl's C caller */
/* ========================================================================== */

int touch_engine_init(const char *id, char *err, size_t errlen, int want_queue)
{
    if (s_inited) {
        snprintf(err, errlen, "touch: already initialized");
        return -1;
    }

    s_queue_consumer = want_queue;

    if (load_cfg(id, err, errlen) != 0) {
        return -1;
    }

    /* Persistent sync primitives (created once, reused across init/deinit). */
    if (!s_sema)          rtos_sema_create(&s_sema, 0, 1);
    if (!s_task_done)     rtos_sema_create(&s_task_done, 0, 1);
    if (!s_queue)         rtos_queue_create(&s_queue, CLAW_TOUCH_EVENT_QUEUE_DEPTH,
                                            sizeof(touch_event_t));
    if (!s_snapshot_lock) rtos_mutex_create(&s_snapshot_lock);
    if (!s_sema || !s_task_done || !s_queue || !s_snapshot_lock) {
        snprintf(err, errlen, "touch: sync primitive alloc failed");
        return -1;
    }

    /* Chip bring-up: GPIO reset+address (chip), I2C bus (generic), probe (chip). */
    s_ops->reset(&s_ctx);
    touch_i2c_bringup(&s_ctx);
    rtos_time_delay_ms(100);            /* chip internal init before first I2C   */
    if (s_ops->probe(&s_ctx) != 0) {
        i2c_reset(&s_ctx.i2c);
        snprintf(err, errlen, "touch: IC not responding on I2C (check wiring/address)");
        return -1;
    }

    /* Drain any stale ready flag + queued events; reset FSM. */
    touch_pt_t tmp;
    (void)s_ops->read_points(&s_ctx, &tmp, 1);
    { touch_event_t e; while (rtos_queue_receive(s_queue, &e, 0) == RTK_SUCCESS) {} }
    snapshot_set(0, 0, 0);

    /* Arm INT (rising edge) → touch_irq_handler → sema. */
    if (gpio_irq_init(&s_irq, (PinName)s_ctx.int_pin, touch_irq_handler, 0) != 0) {
        snprintf(err, errlen, "touch: gpio_irq_init failed");
        return -1;
    }
    gpio_irq_set(&s_irq, IRQ_RISE, 1);
    gpio_irq_enable(&s_irq);

    /* Start the reader task. */
    s_running = 1;
    if (rtos_task_create(NULL, "touch_reader", touch_reader_task, NULL,
                         CLAW_TOUCH_READER_TASK_STACK,
                         CLAW_TOUCH_READER_TASK_PRIO) != RTK_SUCCESS) {
        s_running = 0;
        gpio_irq_disable(&s_irq);
        gpio_irq_deinit(&s_irq);
        snprintf(err, errlen, "touch: reader task create failed");
        return -1;
    }

    s_inited = 1;
    return 0;
}

void touch_engine_snapshot(int *x, int *y, int *pressed)
{
    if (!s_inited || !s_snapshot_lock) {
        *x = *y = *pressed = 0;
        return;
    }
    rtos_mutex_take(s_snapshot_lock, RTOS_MAX_DELAY);
    *x = s_emit_x;
    *y = s_emit_y;
    *pressed = s_prev_pressed;
    rtos_mutex_give(s_snapshot_lock);
}

/* ========================================================================== */
/* Lua: touch.init(id)                                                        */
/* ========================================================================== */

static int l_touch_init(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);

    char err[96] = { 0 };
    if (touch_engine_init(id, err, sizeof(err), 1) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }

    /* Anchor a __gc sentinel so the engine is released when the owning lua_State
     * is closed even if the script never calls touch.deinit() (stop/timeout/return). */
    sentinel_create(L);

    lua_pushboolean(L, 1);
    return 1;
}

/* ========================================================================== */
/* Resource release (shared by touch.deinit() and the __gc sentinel)          */
/* ========================================================================== */

/* Tear down the INT ISR + reader task and clear the inited flag.  Idempotent:
 * a no-op when already released, so an explicit deinit() followed by the
 * sentinel __gc (or vice-versa) is safe. */
static void touch_release(void)
{
    if (!s_inited) {
        return;
    }

    gpio_irq_disable(&s_irq);
    s_running = 0;
    rtos_sema_give(s_sema);                  /* wake the task so it observes !running */
    rtos_sema_take(s_task_done, 1000);       /* wait for clean exit (<=1s)            */

    gpio_irq_deinit(&s_irq);
    /* Leave the I2C bus and the panel powered/running, mirroring the board-proven
     * SDK deinit (input_touch_gt911.c: only the IRQ is torn down).  An earlier
     * version forced RST low + i2c_reset() here, which left the controller in a
     * state where the GT911 no longer ACKed its address on the next init()
     * (TX_ABRT).  init()'s ops->reset re-latches the panel; the I2C controller is
     * re-armed by i2c_init().  Note i2c_init() is NOT idempotent w.r.t. the target
     * address (see the IC_TAR re-sync in touch_i2c_bringup) — a plain re-init would
     * otherwise NAK on the second bring-up. */
    s_inited = 0;
}

void touch_engine_deinit(void)
{
    touch_release();
}

/* ---- __gc sentinel: guarantees release on Lua state teardown ─────────────────
 * The engine (INT ISR + reader task + I2C) lives in C statics decoupled from the
 * owning lua_State, so a script that never reaches its own touch.deinit() (killed
 * by lua_job_stop, a wall-clock timeout, or simply returning from run()) would
 * otherwise leak the IRQ/bus and make the next init() fail with "already
 * initialized".  touch.init() anchors a sentinel userdata in the registry; when
 * lua_close() collects it, __gc runs touch_release().  Mirrors the display
 * backend's ownership sentinel (display_lua.c).  No ABA token is needed: touch has
 * a single global owner (s_inited gates init), the sentinel stays reachable via
 * luaL_ref until the state closes (so __gc never fires mid-session), and
 * touch_release() is idempotent. */
#define TOUCH_SENTINEL_MT "touch.sentinel"

static int sentinel_gc(lua_State *L)
{
    (void)L;
    touch_release();
    return 0;
}

static void sentinel_create(lua_State *L)
{
    (void)lua_newuserdata(L, sizeof(char));
    luaL_getmetatable(L, TOUCH_SENTINEL_MT);
    lua_setmetatable(L, -2);
    luaL_ref(L, LUA_REGISTRYINDEX);   /* keep it alive until the state is closed */
}

/* ========================================================================== */
/* Lua: touch.deinit()                                                        */
/* ========================================================================== */

static int l_touch_deinit(lua_State *L)
{
    (void)L;
    touch_release();
    return 0;
}

/* ========================================================================== */
/* Lua: touch.get_event()  — non-blocking; drains one queued event or nil     */
/* ========================================================================== */

static int l_touch_get_event(lua_State *L)
{
    if (!s_inited) {
        return luaL_error(L, "touch: not initialized (call touch.init first)");
    }

    touch_event_t ev;
    if (rtos_queue_receive(s_queue, &ev, 0) != RTK_SUCCESS) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 5);
    lua_pushstring(L, ev.type < 3 ? s_ev_names[ev.type] : "?");
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, ev.x);  lua_setfield(L, -2, "x");
    lua_pushinteger(L, ev.y);  lua_setfield(L, -2, "y");
    lua_pushinteger(L, ev.dx); lua_setfield(L, -2, "dx");
    lua_pushinteger(L, ev.dy); lua_setfield(L, -2, "dy");
    return 1;
}

static int l_touch_width(lua_State *L)  { lua_pushinteger(L, s_ctx.width);  return 1; }
static int l_touch_height(lua_State *L) { lua_pushinteger(L, s_ctx.height); return 1; }

/* ========================================================================== */
/* Module open                                                                */
/* ========================================================================== */

int luaopen_touch(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "init",      l_touch_init },
        { "deinit",    l_touch_deinit },
        { "get_event", l_touch_get_event },
        { "width",     l_touch_width },
        { "height",    l_touch_height },
        { NULL, NULL }
    };
    /* sentinel metatable (registered once) — see sentinel_create/__gc above. */
    if (luaL_newmetatable(L, TOUCH_SENTINEL_MT)) {
        lua_pushcfunction(L, sentinel_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    luaL_newlib(L, fns);
    return 1;
}
