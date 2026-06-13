/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** luusb_msc.c — Lua usb_msc module for Ameba RTOS (ameba_claw).
**
** Provides require("usb_msc"):
**   usb_msc.init()                        → true | error
**   usb_msc.is_ready()                    → bool
**   usb_msc.wait_ready([timeout_ms=10000])→ true | nil, "timeout"
**   usb_msc.mount()                       → drive_path_string | nil, err
**   usb_msc.umount()                      → true
**   usb_msc.read_file(path)               → data_string | nil, err
**   usb_msc.write_file(path, data)        → true | nil, err
**   usb_msc.list_dir([path])              → [{name, size, is_dir}, ...] | nil, err
**   usb_msc.remove(path)                  → true | nil, err
**   usb_msc.deinit()                      → true
**
** NOTE: Only one USB host module (usb_uvc or usb_msc) may be active at a time.
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "usbh.h"
#include "usbh_msc.h"
#include "usbh_msc_disk.h"
#include "ff.h"
#include "os_wrapper.h"

#include <string.h>
#include <stdlib.h>

#define MSC_READ_MAX  (512 * 1024)

static usbh_config_t s_usbh_cfg = {
	.speed                = USB_SPEED_HIGH,
	.ext_intr_enable      = USBH_SOF_INTR,
	.isr_priority         = INT_PRI_MIDDLE,
	.main_task_stack_size = 768U,
	.main_task_priority   = 3U,
	.tick_source          = USBH_SOF_TICK,
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

static volatile int  s_inited;
static volatile int  s_is_ready;
static volatile int  s_mounted;
static int           s_drv_num;
static FATFS         s_fs;
static char          s_drive[4];

static int msc_cb_attach(void) { return HAL_OK; }

static int msc_cb_setup(void)
{
	s_is_ready = 1;
	return HAL_OK;
}

static usbh_msc_cb_t s_msc_cb = {
	.attach = msc_cb_attach,
	.setup  = msc_cb_setup,
};

static int msc_host_process(usb_host_t *host, u8 msg)
{
	(void)host;
	if (msg == USBH_MSG_DISCONNECTED) s_is_ready = 0;
	return HAL_OK;
}

static usbh_user_cb_t s_usbh_usr_cb = {
	.process = msc_host_process,
};

static const char *fatfs_strerr(FRESULT r)
{
	switch (r) {
	case FR_OK:           return "ok";
	case FR_DISK_ERR:     return "disk error";
	case FR_NOT_READY:    return "not ready";
	case FR_NO_FILE:      return "no file";
	case FR_NO_PATH:      return "no path";
	case FR_INVALID_NAME: return "invalid name";
	case FR_DENIED:       return "access denied";
	case FR_NO_FILESYSTEM:return "no filesystem";
	default:              return "fatfs error";
	}
}

static int lmsc_init(lua_State *L)
{
	if (s_inited) { lua_pushboolean(L, 1); return 1; }

	s_is_ready = 0; s_mounted = 0; s_drv_num = -1;

	int ret = usbh_init(&s_usbh_cfg, &s_usbh_usr_cb);
	if (ret != HAL_OK)
		return luaL_error(L, "usb_msc: usbh_init failed (%d)", ret);

	usbh_msc_init(&s_msc_cb);

	s_drv_num = FATFS_RegisterDiskDriver(&USB_disk_Driver);
	if (s_drv_num < 0) {
		usbh_msc_deinit(); usbh_deinit();
		return luaL_error(L, "usb_msc: FATFS_RegisterDiskDriver failed");
	}

	s_drive[0] = (char)('0' + s_drv_num);
	s_drive[1] = ':'; s_drive[2] = '/'; s_drive[3] = '\0';
	s_inited = 1;
	lua_pushboolean(L, 1);
	return 1;
}

static int lmsc_is_ready(lua_State *L)
{
	lua_pushboolean(L, s_is_ready ? 1 : 0);
	return 1;
}

static int lmsc_wait_ready(lua_State *L)
{
	if (!s_inited) return luaL_error(L, "usb_msc: not initialized");
	u32 timeout = (u32)luaL_optinteger(L, 1, 10000);
	u32 elapsed = 0;
	while (!s_is_ready && elapsed < timeout) {
		rtos_time_delay_ms(100);
		elapsed += 100;
	}
	if (!s_is_ready) {
		lua_pushnil(L);
		lua_pushstring(L, "timeout");
		return 2;
	}
	rtos_time_delay_ms(10);
	lua_pushboolean(L, 1);
	return 1;
}

static int lmsc_mount(lua_State *L)
{
	if (!s_inited) return luaL_error(L, "usb_msc: not initialized");
	if (!s_is_ready) { lua_pushnil(L); lua_pushstring(L, "drive not ready"); return 2; }
	if (s_mounted) { lua_pushstring(L, s_drive); return 1; }

	/* Retry up to 3 times with 100ms gap — some drives need extra settling time */
	FRESULT r = FR_NOT_READY;
	for (int i = 0; i < 3 && r != FR_OK; i++) {
		if (i > 0) rtos_time_delay_ms(100);
		r = f_mount(&s_fs, s_drive, 1);
	}
	if (r != FR_OK) { lua_pushnil(L); lua_pushfstring(L, "%s (err=%d)", fatfs_strerr(r), (int)r); return 2; }
	s_mounted = 1;
	lua_pushstring(L, s_drive);
	return 1;
}

static int lmsc_umount(lua_State *L)
{
	if (s_mounted) { f_unmount(s_drive); s_mounted = 0; }
	lua_pushboolean(L, 1);
	return 1;
}

static int lmsc_read_file(lua_State *L)
{
	if (!s_inited || !s_mounted) return luaL_error(L, "usb_msc: not mounted");
	const char *path = luaL_checkstring(L, 1);

	FIL f;
	FRESULT r = f_open(&f, path, FA_READ);
	if (r != FR_OK) { lua_pushnil(L); lua_pushfstring(L, "open '%s': %s", path, fatfs_strerr(r)); return 2; }

	FSIZE_t fsize = f_size(&f);
	if (fsize == 0) { f_close(&f); lua_pushlstring(L, "", 0); return 1; }
	if (fsize > MSC_READ_MAX) {
		f_close(&f);
		lua_pushnil(L); lua_pushfstring(L, "file too large (%d, max %d)", (int)fsize, MSC_READ_MAX); return 2;
	}

	char *buf = (char *)malloc((size_t)fsize);
	if (!buf) { f_close(&f); return luaL_error(L, "usb_msc: out of memory"); }

	UINT br;
	r = f_read(&f, buf, (UINT)fsize, &br);
	f_close(&f);

	if (r != FR_OK) { free(buf); lua_pushnil(L); lua_pushfstring(L, "read '%s': %s", path, fatfs_strerr(r)); return 2; }
	lua_pushlstring(L, buf, (size_t)br);
	free(buf);
	return 1;
}

static int lmsc_write_file(lua_State *L)
{
	if (!s_inited || !s_mounted) return luaL_error(L, "usb_msc: not mounted");
	const char *path = luaL_checkstring(L, 1);
	size_t len; const char *data = luaL_checklstring(L, 2, &len);

	FIL f;
	FRESULT r = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
	if (r != FR_OK) { lua_pushnil(L); lua_pushfstring(L, "open '%s': %s", path, fatfs_strerr(r)); return 2; }

	UINT bw;
	r = f_write(&f, data, (UINT)len, &bw);
	f_close(&f);

	if (r != FR_OK || bw != (UINT)len) {
		lua_pushnil(L); lua_pushfstring(L, "write '%s': %d/%d (%s)", path, (int)bw, (int)len, fatfs_strerr(r)); return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int lmsc_list_dir(lua_State *L)
{
	if (!s_inited || !s_mounted) return luaL_error(L, "usb_msc: not mounted");
	const char *path = lua_isnoneornil(L, 1) ? s_drive : luaL_checkstring(L, 1);

	DIR dp;
	FRESULT r = f_opendir(&dp, path);
	if (r != FR_OK) { lua_pushnil(L); lua_pushfstring(L, "opendir '%s': %s", path, fatfs_strerr(r)); return 2; }

	lua_newtable(L);
	int idx = 1; FILINFO fno;
	while (1) {
		r = f_readdir(&dp, &fno);
		if (r != FR_OK || fno.fname[0] == '\0') break;
		lua_createtable(L, 0, 3);
		lua_pushstring(L, fno.fname);   lua_setfield(L, -2, "name");
		lua_pushinteger(L, (lua_Integer)fno.fsize); lua_setfield(L, -2, "size");
		lua_pushboolean(L, (fno.fattrib & AM_DIR) ? 1 : 0); lua_setfield(L, -2, "is_dir");
		lua_rawseti(L, -2, idx++);
	}
	f_closedir(&dp);
	return 1;
}

static int lmsc_remove(lua_State *L)
{
	if (!s_inited || !s_mounted) return luaL_error(L, "usb_msc: not mounted");
	const char *path = luaL_checkstring(L, 1);
	FRESULT r = f_unlink(path);
	if (r != FR_OK) { lua_pushnil(L); lua_pushfstring(L, "remove '%s': %s", path, fatfs_strerr(r)); return 2; }
	lua_pushboolean(L, 1);
	return 1;
}

static int lmsc_deinit(lua_State *L)
{
	if (!s_inited) { lua_pushboolean(L, 1); return 1; }
	if (s_mounted) { f_unmount(s_drive); s_mounted = 0; }
	if (s_drv_num >= 0) { FATFS_UnRegisterDiskDriver(s_drv_num); s_drv_num = -1; }
	usbh_msc_deinit();
	usbh_deinit();
	s_is_ready = 0; s_inited = 0;
	lua_pushboolean(L, 1);
	return 1;
}

static const luaL_Reg msc_lib[] = {
	{"init",       lmsc_init},
	{"is_ready",   lmsc_is_ready},
	{"wait_ready", lmsc_wait_ready},
	{"mount",      lmsc_mount},
	{"umount",     lmsc_umount},
	{"read_file",  lmsc_read_file},
	{"write_file", lmsc_write_file},
	{"list_dir",   lmsc_list_dir},
	{"remove",     lmsc_remove},
	{"deinit",     lmsc_deinit},
	{NULL, NULL}
};

LUAMOD_API int luaopen_usb_msc(lua_State *L)
{
	luaL_newlib(L, msc_lib);
	return 1;
}
