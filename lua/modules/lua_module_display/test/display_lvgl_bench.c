/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * display_lvgl_bench.c — one-shot FPS benchmark for the *Lua/LVGL* display path
 * (the "display" module + its LCDC/RGB backend), driven from a hardcoded Lua
 * script so a single AT command exercises the exact render pipeline the REPL
 * does: require('display') → init → clear/fill_circle → present_full.
 *
 * Entry: display_lvgl_bench_run(frames, dev_id) — wired to AT+CLAW=display_bench
 * in atcmd_hw_test.c.  The animation is now an ENDLESS physics demo, so this
 * never returns (reset the board to stop); `frames` is ignored.  Live FPS is
 * shown in the on-screen header rather than printed at the end.
 *
 * Mirrors lua_run_repl_once() (lua_repl.c): a throw-away lua_State +
 * luaL_openlibs installs every REPL module (display, sys, math, string, ...);
 * closing the state runs the display sentinel __gc → releases the panel.  This
 * is the SAME task/stack the REPL uses, so it is known to hold an LVGL init +
 * software-render draw.  Do NOT feed this script to the agent LLM — it is a
 * developer bench, not an assistant capability.
 */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stdio.h>

/* The benchmark body.  Reads one global injected from C before it runs:
 *   BENCH_DEV : board.json device id to init (string)
 * An ENDLESS physics demo (never returns — reset the board to stop): eight
 * spring balls under weak gravity bounce elastically off the walls and each
 * other.  Each ball leaves a fading motion trail (拖影); ball-ball impacts throw
 * a burst of sparks; wall impacts emit an expanding arc shock ring.  A gradient
 * + grid background sits under a header bar carrying the title and a live FPS.
 * This is the heaviest render the path takes (direct clear + gradient fills +
 * dozens of AA circles/arcs/lines + text per frame), so the on-screen FPS is a
 * realistic worst-case for the Lua/LVGL/LCDC pipeline. */
static const char *BENCH_LUA =
    "local d = require('display')\n"
    "local s = require('sys')\n"
    "local ok, err = d.init(BENCH_DEV)\n"
    "if not ok then print('[bench] display init failed:', err) return end\n"
    "local W, H = d.width, d.height\n"
    "local sin, cos, floor, sqrt, rnd = math.sin, math.cos, math.floor, math.sqrt, math.random\n"
    "math.randomseed(2026)\n"
    "local HH = 44\n"                       /* header height */
    "local top, bot = HH + 3, H - 3\n"
    "local left, right = 3, W - 3\n"
    "local GY = 0.12\n"                     /* weak gravity per frame */
    "local TRAIL = 6\n"
    "local pcols = {0xFF4060,0x40FF80,0x4080FF,0xFFD040,0xFF60FF,0x40FFFF,0xFF9040,0xA0FF40}\n"
    /* dim toward black (f=0..1) */
    "local function dim(c, f)\n"
    "  if f < 0 then f = 0 elseif f > 1 then f = 1 end\n"
    "  local r = floor(((c >> 16) & 0xFF) * f)\n"
    "  local g = floor(((c >> 8) & 0xFF) * f)\n"
    "  local b = floor((c & 0xFF) * f)\n"
    "  return (r << 16) | (g << 8) | b\n"
    "end\n"
    /* blend toward white (f=0..1) — used for the specular highlight */
    "local function lite(c, f)\n"
    "  local r = (c >> 16) & 0xFF\n"
    "  local g = (c >> 8) & 0xFF\n"
    "  local b = c & 0xFF\n"
    "  r = floor(r + (255 - r) * f); g = floor(g + (255 - g) * f); b = floor(b + (255 - b) * f)\n"
    "  return (r << 16) | (g << 8) | b\n"
    "end\n"
    "local balls = {}\n"
    "for i = 1, 8 do\n"
    "  balls[i] = { x = left + 30 + (i * 53) % (right - left - 60),\n"
    "               y = top + 20 + (i * 37) % 120,\n"
    "               vx = ((i % 2 == 0) and 2.4 or -2.1) + i * 0.15,\n"
    "               vy = -1.5 - (i % 3) * 0.8,\n"
    "               r = 12 + (i % 3) * 4, c = pcols[i], tr = {} }\n"
    "end\n"
    "local sparks, rings = {}, {}\n"
    "local function add_ring(x, y, a, c)\n"
    "  if #rings < 40 then rings[#rings+1] = {x=x, y=y, r=10, a=a, life=12, c=c} end\n"
    "end\n"
    "local function add_sparks(x, y, c)\n"
    "  if #sparks > 90 then return end\n"
    "  for k = 1, 9 do\n"
    "    local an, sp = rnd() * 6.28, 1.5 + rnd() * 3.0\n"
    "    sparks[#sparks+1] = {x=x, y=y, vx=cos(an)*sp, vy=sin(an)*sp,\n"
    "                         life=10 + floor(rnd()*6), c=(k % 2 == 0) and 0xFFFFFF or c}\n"
    "  end\n"
    "end\n"
    "local prev, fps = s.millis(), 0.0\n"
    "local comp = 0\n"
    "while true do\n"
    "  local c0 = s.millis()\n"
    /* ---- physics: gravity + integrate + wall bounce + trail ---- */
    "  for i = 1, 8 do\n"
    "    local b = balls[i]\n"
    "    b.vy = b.vy + GY\n"
    "    b.x = b.x + b.vx; b.y = b.y + b.vy\n"
    "    if b.x - b.r < left  then b.x = left + b.r;  b.vx = -b.vx; add_ring(left, b.y, 0, b.c) end\n"
    "    if b.x + b.r > right then b.x = right - b.r; b.vx = -b.vx; add_ring(right, b.y, 180, b.c) end\n"
    "    if b.y - b.r < top   then b.y = top + b.r;   b.vy = -b.vy; add_ring(b.x, top, 90, b.c) end\n"
    "    if b.y + b.r > bot   then b.y = bot - b.r;   b.vy = -b.vy; add_ring(b.x, bot, 270, b.c) end\n"
    "    local tr = b.tr\n"
    "    tr[#tr+1] = {b.x, b.y}\n"
    "    if #tr > TRAIL then table.remove(tr, 1) end\n"
    "  end\n"
    /* ---- ball-ball elastic collision (equal mass) + sparks ---- */
    "  for i = 1, 7 do\n"
    "    local a = balls[i]\n"
    "    for j = i + 1, 8 do\n"
    "      local b = balls[j]\n"
    "      local dx, dy = b.x - a.x, b.y - a.y\n"
    "      local dist = sqrt(dx*dx + dy*dy)\n"
    "      local mind = a.r + b.r\n"
    "      if dist > 0.1 and dist < mind then\n"
    "        local nx, ny = dx / dist, dy / dist\n"
    "        local ov = (mind - dist) / 2\n"
    "        a.x = a.x - nx*ov; a.y = a.y - ny*ov\n"
    "        b.x = b.x + nx*ov; b.y = b.y + ny*ov\n"
    "        local vn = (b.vx - a.vx)*nx + (b.vy - a.vy)*ny\n"
    "        if vn < 0 then\n"
    "          a.vx = a.vx + vn*nx; a.vy = a.vy + vn*ny\n"
    "          b.vx = b.vx - vn*nx; b.vy = b.vy - vn*ny\n"
    "          add_sparks(a.x + nx*a.r, a.y + ny*a.r, a.c)\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    /* ---- advance sparks / rings, swap-remove the dead ---- */
    "  for k = #sparks, 1, -1 do\n"
    "    local p = sparks[k]\n"
    "    p.vy = p.vy + 0.08; p.x = p.x + p.vx; p.y = p.y + p.vy; p.life = p.life - 1\n"
    "    if p.life <= 0 then sparks[k] = sparks[#sparks]; sparks[#sparks] = nil end\n"
    "  end\n"
    "  for k = #rings, 1, -1 do\n"
    "    local rg = rings[k]\n"
    "    rg.r = rg.r + 4; rg.life = rg.life - 1\n"
    "    if rg.life <= 0 then rings[k] = rings[#rings]; rings[#rings] = nil end\n"
    "  end\n"
    /* ---- render: background gradient + grid ---- */
    "  d.clear(0x05060d)\n"
    "  local NB = 9\n"
    "  local bh = floor(H / NB) + 1\n"
    "  for bi = 0, NB - 1 do\n"
    "    d.fill_rect(0, bi*bh, W, bh, dim(0x1a2f5a, 0.25 + (bi/(NB-1)) * 0.55))\n"
    "  end\n"
    "  for gx = 0, W, 80 do d.draw_line(gx, HH, gx, H, 0x14203a, 1) end\n"
    "  for gy = HH, H, 80 do d.draw_line(0, gy, W, gy, 0x14203a, 1) end\n"
    /* ---- wall shock rings (expanding arc facing away from wall) ---- */
    "  for k = 1, #rings do\n"
    "    local rg = rings[k]\n"
    "    d.draw_arc(rg.x, rg.y, rg.r, rg.a - 70, rg.a + 70, dim(rg.c, rg.life / 12), 3)\n"
    "  end\n"
    /* ---- motion trails (拖影): ONE batched direct-write fill_circles.  Trails
     * are small, dim and moving, so the lack of AA is invisible — keep them on
     * the cheap path.  Flat {cx,cy,r,color,...}, oldest-dim -> newest. ---- */
    "  local cir = {}\n"
    "  local ci = 0\n"
    "  for i = 1, 8 do\n"
    "    local b = balls[i]\n"
    "    local tr = b.tr\n"
    "    local n = #tr\n"
    "    for t = 1, n do\n"
    "      local f = t / n\n"
    "      cir[ci+1]=floor(tr[t][1]); cir[ci+2]=floor(tr[t][2])\n"
    "      cir[ci+3]=floor(b.r*(0.35+0.5*f)); cir[ci+4]=dim(b.c, f*0.5); ci=ci+4\n"
    "    end\n"
    "  end\n"
    "  d.fill_circles(cir)\n"
    /* ---- balls + specular highlight: the prominent big circles.  fill_circles
     * is anti-aliased too now (coverage-blended rim), so batch each ball + its
     * highlight into one call — 16 circles/frame, smooth edges, composited on
     * TOP of the direct trails. */
    "  for i = 1, 8 do\n"
    "    local b = balls[i]\n"
    "    d.fill_circles({floor(b.x), floor(b.y), b.r, b.c,\n"
    "                    floor(b.x - b.r*0.32), floor(b.y - b.r*0.32), floor(b.r*0.35), lite(b.c, 0.6)})\n"
    "  end\n"
    /* ---- sparks: AA streaks first, then all head dots in one batched fill_circles ---- */
    "  local dots = {}\n"
    "  local di = 0\n"
    "  for k = 1, #sparks do\n"
    "    local p = sparks[k]\n"
    "    local c = dim(p.c, p.life / 16)\n"
    "    d.draw_line(floor(p.x), floor(p.y), floor(p.x - p.vx*1.5), floor(p.y - p.vy*1.5), c, 2)\n"
    "    dots[di+1]=floor(p.x); dots[di+2]=floor(p.y); dots[di+3]=2; dots[di+4]=p.c; di=di+4\n"
    "  end\n"
    "  d.fill_circles(dots)\n"
    /* ---- header bar (drawn last, on top) + live FPS ---- */
    "  d.fill_rect(0, 0, W, HH, 0x0c1430)\n"
    "  d.fill_round_rect(6, 6, W - 12, HH - 12, 8, 0x18305e)\n"
    "  d.draw_line(0, HH, W, HH, 0x4a80ff, 2)\n"
    "  local now = s.millis()\n"
    "  local dtf = now - prev; prev = now\n"
    "  if dtf > 0 then fps = fps * 0.9 + (1000.0 / dtf) * 0.1 end\n"
    "  d.draw_text(16, 12, 'SPRING BALLS', 0xFFFFFF, nil, 20)\n"
    /* live readout: FPS + prev frame's render(R=AA raster) / push(P=DMA+vsync) ms */
    "  d.draw_text(W - 260, 15, string.format('FPS %.0f C%d R%d P%d', fps, comp, d.render_ms(), d.frame_ms()), 0x9fd0ff, nil, 16)\n"
    "  comp = s.millis() - c0\n"   /* Lua compute + all draw-call enqueue for THIS frame */
    "  d.present_full()\n"
    "end\n";

/* Static-frame diagnostic: init the SAME Lua/LVGL path, then show three solid
 * frames (red/green/blue).  After each present_full the CPU does NOTHING but
 * sleep — the scan-out DMA owns the framebuffer undisturbed.  If a static frame
 * is clean while an animated one tears at the top, the artifact is single-
 * buffer CPU/DMA contention (not a framebuffer-content/DCache bug). */
static const char *STATIC_LUA =
    "local d = require('display')\n"
    "local s = require('sys')\n"
    "local hold = BENCH_HOLD or 3000\n"
    "local ok, err = d.init(BENCH_DEV)\n"
    "if not ok then print('[static] init failed:', err) return end\n"
    "local frames = {{'red', 0xFF0000}, {'green', 0x00FF00}, {'blue', 0x0000FF}}\n"
    "for i = 1, 3 do\n"
    "  d.clear(frames[i][2])\n"
    "  d.present_full()\n"
    "  print(string.format('[static] %s held %d ms (no CPU writes)',\n"
    "                      frames[i][1], hold))\n"
    "  s.sleep_ms(hold)\n"
    "end\n"
    "print('[static] done')\n";

void display_lvgl_static_run(int hold_ms, const char *dev_id)
{
    if (hold_ms <= 0) {
        hold_ms = 3000;
    }
    if (!dev_id || !dev_id[0]) {
        dev_id = "display_lcd_rgb_st7701p";
    }

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[static] failed to create Lua state\n");
        return;
    }
    luaL_openlibs(L);

    lua_pushinteger(L, hold_ms);
    lua_setglobal(L, "BENCH_HOLD");
    lua_pushstring(L, dev_id);
    lua_setglobal(L, "BENCH_DEV");

    if (luaL_dostring(L, STATIC_LUA) != LUA_OK) {
        printf("[static] Lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_close(L);   /* sentinel __gc releases the display */
}

void display_lvgl_bench_run(int frames, const char *dev_id)
{
    if (frames <= 0) {
        frames = 600;
    }
    if (!dev_id || !dev_id[0]) {
        dev_id = "display_lcd_rgb_st7701p";
    }

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[bench] failed to create Lua state\n");
        return;
    }
    /* Same rationale as lua_run_repl_once(): openlibs installs the REPL module
     * set (incl. 'display' and 'sys'); driver mutexes were provisioned at boot
     * and their _init guards make a second openlibs safe. */
    luaL_openlibs(L);

    lua_pushinteger(L, frames);
    lua_setglobal(L, "BENCH_FRAMES");
    lua_pushstring(L, dev_id);
    lua_setglobal(L, "BENCH_DEV");

    if (luaL_dostring(L, BENCH_LUA) != LUA_OK) {
        printf("[bench] Lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_close(L);   /* sentinel __gc releases the display */
}
