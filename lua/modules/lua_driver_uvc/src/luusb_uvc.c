/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** luusb_uvc.c — Lua usb_uvc module for Ameba RTOS (ameba_claw).
**
** Provides require("usb_uvc"):
**   usb_uvc.init()                        → true | error
**   usb_uvc.wait_ready([timeout_ms=10000])→ true | nil, "timeout"
**   usb_uvc.set_param(opts)               → true | nil, err
**     opts: {width=640, height=480, fps=15, format="mjpeg", buf_size=153600}
**     format: "mjpeg" (default) | "yuv" | "h264"
**   usb_uvc.stream_on()                   → true | nil, err
**   usb_uvc.get_frame([timeout_ms=1000])  → data_string | nil, err
**   usb_uvc.stream_off()                  → true
**   usb_uvc.deinit()                      → true
**
** NOTE: Only one USB host module (usb_uvc or usb_msc) may be active at a time.
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "usbh.h"
#include "usbh_uvc_intf.h"
#include "usbh_uvc.h"   /* usbh_uvc_host_t, UVC_STATE_*, STREAM_STATE_* */
#include "os_wrapper.h"

#include <string.h>

/* Access internal UVC host state to drive the SET_INTERFACE(alt=0) state machine. */
extern usbh_uvc_host_t uvc_host;

/* ---- RTL8721F USB host FIFO config ---- */
static usbh_config_t s_usbh_cfg = {
	.speed                = USB_SPEED_HIGH,
	.ext_intr_enable      = 0,           /* no SOF interrupt: avoids WiFi IPC contention */
	.isr_priority         = INT_PRI_LOWEST,
	.main_task_stack_size = 1024U,       /* match official verification test config */
	.main_task_priority   = 3U,
	.tick_source          = USBH_SW_TICK, /* software tick: no periodic SOF interrupt */
#if defined(CONFIG_AMEBAGREEN2)
	.rx_fifo_depth  = 500,
	.nptx_fifo_depth = 256,
	.ptx_fifo_depth  = 256,
#elif defined(CONFIG_AMEBAL2)
	.rx_fifo_depth  = 501,
	.nptx_fifo_depth = 256,
	.ptx_fifo_depth  = 256,
#elif defined(CONFIG_AMEBAPRO3)
	.rx_fifo_depth  = 1712,
	.nptx_fifo_depth = 256,
	.ptx_fifo_depth  = 256,
#endif
};

/* ---- Module state ---- */
static volatile int      s_inited;
static volatile int      s_streaming;
static rtos_sema_t       s_setup_sema;
static rtos_sema_t       s_setparam_sema;
static usbh_uvc_s_ctx_t  s_stream_ctx;
/* hw_isr_pri must be INT_PRI_LOWEST: WiFi IPC runs at INT_PRI5, so UVC DEC IRQ at
 * INT_PRI_HIGHEST (0, the zero-init default) would starve WiFi IPC → IPC timeouts. */
static usbh_uvc_ctx_t    s_uvc_ctx = { .hw_isr_pri = INT_PRI_LOWEST };

/* ---- UVC callbacks (called from USB host task, NOT ISR) ---- */
static int uvc_cb_init(void)    { return HAL_OK; }
static int uvc_cb_deinit(void)  { return HAL_OK; }
static int uvc_cb_attach(void)  { return HAL_OK; }
static int uvc_cb_detach(void)  { return HAL_OK; }

static int uvc_cb_setup(void)
{
	rtos_sema_give(s_setup_sema);
	return HAL_OK;
}

static int uvc_cb_setparam(int status)
{
	(void)status;
	rtos_sema_give(s_setparam_sema);
	return HAL_OK;
}

static usbh_uvc_cb_t s_uvc_cb = {
	.init      = uvc_cb_init,
	.deinit    = uvc_cb_deinit,
	.attach    = uvc_cb_attach,
	.detach    = uvc_cb_detach,
	.setup     = uvc_cb_setup,
	.set_param = uvc_cb_setparam,
};

/* ---- Lua API ---- */

/*
 * Send SET_INTERFACE(bAlternateSetting=0) to the VS interface via the existing
 * state machine, then wait (max 2 s) for the UVC main task to process it.
 *
 * Hikvision (and many other UVC cameras) run isochronous DMA on the device side
 * even after the host closes the ISO pipe.  Without this request the camera
 * firmware never receives the "stop streaming" signal, gets stuck, and does not
 * respond correctly when the host reconnects — even after a SOC reset, because
 * board VBUS stays powered and the camera never loses power.
 */
static void luvc_send_set_interface_zero(void)
{
	usbh_uvc_host_t *uvc = &uvc_host;

	/* Only meaningful when ISO was actually started (state == TRANSFER). */
	if (!uvc->host || uvc->state != UVC_STATE_TRANSFER) {
		return;
	}

	uvc->state              = UVC_STATE_CTRL;
	uvc->stream[0].state    = STREAM_STATE_SET_PARA;
	uvc->stream_ctrl_idx    = 0;
	usbh_notify_class_state_change(uvc->host, 0);

	/* Poll until the state machine completes SET_INTERFACE(0) → UVC_STATE_IDLE. */
	int ms = 2000;
	while (uvc->state != UVC_STATE_IDLE && ms > 0) {
		rtos_time_delay_ms(10);
		ms -= 10;
	}
}

static int luvc_init(lua_State *L)
{
	if (s_inited) { lua_pushboolean(L, 1); return 1; }

	/* Do NOT memset s_uvc_ctx: that would reset hw_isr_pri to 0 (= INT_PRI_HIGHEST)
	 * on subsequent calls, causing the HW UVC decoder ISR to starve WiFi IPC. */
	memset(&s_stream_ctx, 0, sizeof(s_stream_ctx));

	rtos_sema_create(&s_setup_sema,    0U, 1U);
	rtos_sema_create(&s_setparam_sema, 0U, 1U);

	int ret = usbh_init(&s_usbh_cfg, NULL);
	if (ret != HAL_OK) {
		rtos_sema_delete(s_setup_sema);
		rtos_sema_delete(s_setparam_sema);
		return luaL_error(L, "usb_uvc: usbh_init failed (%d)", ret);
	}

	ret = usbh_uvc_init(&s_uvc_ctx, &s_uvc_cb);
	if (ret != HAL_OK) {
		usbh_deinit();
		rtos_sema_delete(s_setup_sema);
		rtos_sema_delete(s_setparam_sema);
		return luaL_error(L, "usb_uvc: usbh_uvc_init failed (%d)", ret);
	}

	s_inited    = 1;
	s_streaming = 0;
	lua_pushboolean(L, 1);
	return 1;
}

static int luvc_wait_ready(lua_State *L)
{
	if (!s_inited) return luaL_error(L, "usb_uvc: not initialized");
	u32 timeout = (u32)luaL_optinteger(L, 1, 10000);
	if (rtos_sema_take(s_setup_sema, timeout) != RTK_SUCCESS) {
		lua_pushnil(L);
		lua_pushstring(L, "timeout");
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int luvc_set_param(lua_State *L)
{
	if (!s_inited) return luaL_error(L, "usb_uvc: not initialized");
	luaL_checktype(L, 1, LUA_TTABLE);

	lua_getfield(L, 1, "width");
	s_stream_ctx.width = (u16)luaL_optinteger(L, -1, 640);
	lua_pop(L, 1);

	lua_getfield(L, 1, "height");
	s_stream_ctx.height = (u16)luaL_optinteger(L, -1, 480);
	lua_pop(L, 1);

	lua_getfield(L, 1, "fps");
	s_stream_ctx.frame_rate = (u8)luaL_optinteger(L, -1, 15);
	lua_pop(L, 1);

	lua_getfield(L, 1, "buf_size");
	s_stream_ctx.frame_buf_size = (u32)luaL_optinteger(L, -1, 150 * 1024);
	lua_pop(L, 1);

	lua_getfield(L, 1, "format");
	s_stream_ctx.fmt_type = USBH_UVC_FORMAT_MJPEG;
	if (lua_isstring(L, -1)) {
		const char *s = lua_tostring(L, -1);
		if (strcmp(s, "yuv") == 0)       s_stream_ctx.fmt_type = USBH_UVC_FORMAT_YUV;
		else if (strcmp(s, "h264") == 0) s_stream_ctx.fmt_type = USBH_UVC_FORMAT_H264;
	}
	lua_pop(L, 1);

	int ret = usbh_uvc_set_param(&s_stream_ctx, 0);
	if (ret != RTK_SUCCESS) {
		lua_pushnil(L);
		lua_pushfstring(L, "set_param request failed (%d)", ret);
		return 2;
	}

	/* usbh_uvc_set_param is async — wait for setparam callback (max 8s) */
	if (rtos_sema_take(s_setparam_sema, 8000) != RTK_SUCCESS) {
		lua_pushnil(L);
		lua_pushstring(L, "set_param timeout");
		return 2;
	}

	lua_pushboolean(L, 1);
	return 1;
}

static int luvc_stream_on(lua_State *L)
{
	if (!s_inited) return luaL_error(L, "usb_uvc: not initialized");
	if (s_streaming) { lua_pushboolean(L, 1); return 1; }

	int ret = usbh_uvc_stream_on(&s_stream_ctx, 0);
	if (ret != RTK_SUCCESS) {
		lua_pushnil(L);
		lua_pushfstring(L, "stream_on failed (%d)", ret);
		return 2;
	}
	s_streaming = 1;
	lua_pushboolean(L, 1);
	return 1;
}

static int luvc_get_frame(lua_State *L)
{
	if (!s_inited || !s_streaming) return luaL_error(L, "usb_uvc: not streaming");
	(void)luaL_optinteger(L, 1, 1000); /* SDK blocks internally up to USBH_UVC_GET_FRAME_TIMEOUT (1000ms) */

	usbh_uvc_frame_t *frame = usbh_uvc_get_frame(0);
	if (frame == NULL) {
		lua_pushnil(L);
		lua_pushstring(L, "timeout");
		return 2;
	}
	if (frame->err || frame->byteused == 0) {
		usbh_uvc_put_frame(frame, 0);
		lua_pushnil(L);
		lua_pushstring(L, "frame error");
		return 2;
	}

	lua_pushlstring(L, (const char *)frame->buf, (size_t)frame->byteused);
	usbh_uvc_put_frame(frame, 0);
	return 1;
}

static int luvc_stream_off(lua_State *L)
{
	if (s_inited && s_streaming) {
		usbh_uvc_stream_off(0);
		s_streaming = 0;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int luvc_deinit(lua_State *L)
{
	if (!s_inited) { lua_pushboolean(L, 1); return 1; }

	/* Tell the camera to stop ISO streaming before closing the pipe.
	 * Without SET_INTERFACE(alt=0) the Hikvision firmware keeps its DMA engine
	 * running and gets stuck; a subsequent host reconnect (or even SOC reset)
	 * fails because board VBUS never drops and the camera is never power-cycled. */
	luvc_send_set_interface_zero();

	if (s_streaming) {
		usbh_uvc_stream_off(0);
		s_streaming = 0;
	}
	usbh_uvc_deinit();
	rtos_time_delay_ms(100); /* let USB main task drain before tearing down host */
	usbh_deinit();
	rtos_sema_delete(s_setup_sema);
	rtos_sema_delete(s_setparam_sema);
	s_inited = 0;
	lua_pushboolean(L, 1);
	return 1;
}

static const luaL_Reg uvc_lib[] = {
	{"init",       luvc_init},
	{"wait_ready", luvc_wait_ready},
	{"set_param",  luvc_set_param},
	{"stream_on",  luvc_stream_on},
	{"get_frame",  luvc_get_frame},
	{"stream_off", luvc_stream_off},
	{"deinit",     luvc_deinit},
	{NULL, NULL}
};

LUAMOD_API int luaopen_usb_uvc(lua_State *L)
{
	luaL_newlib(L, uvc_lib);
	return 1;
}
