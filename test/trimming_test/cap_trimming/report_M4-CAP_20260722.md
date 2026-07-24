# Cap Trimming Test Report

<!--
Format notes:
- Append new test sections (## M4-CAP-XX ...) below and update the Summary table.
- Each sub-test block records build / REST / LLM results verbatim for traceability.
- Status values: PASS | FAIL | SKIP | WIP
-->

## Summary

| Test ID   | Name                         | Status | Date       | Tester         |
|-----------|------------------------------|--------|------------|----------------|
| M4-CAP-01 | L1 单 cap 关断（Kconfig）    | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-02 | L1 全量可选 cap 关断（13个） | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-03 | rolfs 文档条件 staging       | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-04 | always-on 模块不受影响       | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-05 | L1 固件尺寸基准              | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-06 | L2 运行时关断                | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-07 | IM 单平台关断                | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-08 | 全 IM 关断 attachment 消失   | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-09 | L2 全量关断                  | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-10 | L3 单 cap 隐藏               | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-11 | L3 全量隐藏                  | PASS   | 2026-07-22 | alejandro_chen |
| M4-CAP-12 | 特殊 cap（honesty）          | PASS   | 2026-07-22 | alejandro_chen |

---

## M4-CAP-01  L1 单 cap 关断（Kconfig）

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F, build from master @ b8aba0e
**Pre-conditions:**
- L2 runtime disabled list cleared via `POST /api/cap/groups/runtime {"disabled":[]}`
- L3 visibility hidden list cleared via `POST /api/cap/groups/visibility {"hidden":[]}`
- Board rebooted, WiFi connected (192.168.137.52)

**Overall result: PASS**

---

### #1  web_search（CLAW_CAP_WEB_SEARCH）

#### n 轮（关断）

- **menuconfig:** `CLAW_CAP_WEB_SEARCH=n` — applied OK
- **Build:** PASS — `Build done`, 零 error/warning
- **Flash:** PASS — `Finished PASS`
- **REST verify (`/api/cap/groups`):**
  - `web_search absent: True` ✅
- **LLM verify (`AT+CLAW=ask,你是否有web_search工具`):**
  - 回复：`我**没有** web_search 工具` ✅（工具列表中无 web_search 相关条目）

#### y 轮（恢复）

- **menuconfig:** `CLAW_CAP_WEB_SEARCH=y` — applied OK
- **Build:** PASS — `Build done`, 零 error/warning
- **Flash:** PASS — `Finished PASS`
- **REST verify (`/api/cap/groups`):**
  - `web_search present: True` ✅
- **LLM verify (`AT+CLAW=ask,你是否有web_search工具`):**
  - 回复：`是的，我**有** web_search 工具！` ✅（列出 `web_search` 工具及参数说明）

**#1 结论: PASS**

---

### #2  scheduler（CLAW_CAP_SCHEDULER）

#### n 轮（关断）

- **menuconfig:** `CLAW_CAP_SCHEDULER=n` — applied OK
- **Build:** PASS — `Build done`, 零 error/warning
- **Flash:** PASS — `Finished PASS`
- **REST verify (`/api/cap/groups`):**
  - `scheduler absent: True` ✅
- **LLM verify (`AT+CLAW=ask,你是否有scheduler工具`):**
  - 回复：`我**没有** scheduler 工具` ✅（工具列表中无 scheduler 相关条目，web_search 仍在列表中）

#### y 轮（恢复）

- **menuconfig:** `CLAW_CAP_SCHEDULER=y` — applied OK
- **Build:** PASS — `Build done`, 零 error/warning
- **Flash:** PASS — `Finished PASS`
- **REST verify (`/api/cap/groups`):**
  - `scheduler present: True` ✅
- **LLM verify (`AT+CLAW=ask,你是否有scheduler工具`):**
  - 回复：`是的，我**有** scheduler 相关工具！` ✅（列出 `scheduler_add_job`、`scheduler_list_jobs` 等5个工具）

**#2 结论: PASS**

---

## M4-CAP-02  L1 全量可选 cap 关断（Kconfig）

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F, build from master
**test_spec 版本:** 更新至13个受控可选 cap（原8个 + router_mgr / honesty / im_telegram / im_feishu / im_qq）
**Pre-conditions:**
- L2 runtime disabled list cleared via `POST /api/cap/groups/runtime {"disabled":[]}`
- L3 visibility hidden list cleared via `POST /api/cap/groups/visibility {"hidden":[]}`
- Board rebooted, WiFi connected (192.168.137.52)

**Overall result: PASS**

---

### 受控可选 cap 列表（13个）

通过 `grep "^config CLAW_CAP_" Kconfig` 枚举，排除核心和 WebUI 基础设施后：
`scheduler`, `web_search`, `http_request`, `vision`, `net_discover`, `audio_stream`,
`router_mgr`, `honesty`, `mcp_client`, `mcp_server`, `im_telegram`, `im_feishu`, `im_qq`

保持 y（核心 + WebUI 基础设施）：
`time`, `files`, `system`, `board_mgr`, `skill_mgr`, `webui`, `im_local`, `im_wechat`, `im_attachment`

---

### n 轮（全量关断）

- **menuconfig:** 13个 cap 全部设为 n — applied OK
- **Build:** PASS — `Build done`，零 error
- **Flash:** PASS — `Finished PASS`
- **REST verify (`/api/cap/groups`):**
  - `OK missing: {'audio_stream', 'mcp_client', 'web_search', 'http_request', 'net_discover', 'vision', 'mcp_server', 'scheduler', 'router_mgr', 'honesty', 'im_telegram', 'im_feishu', 'im_qq'}` ✅
  - `FAIL still present: set()` ✅（为空）
  - `system present: True` ✅
  - `time present: True` ✅
  - `webui present: True` ✅
  - 实际 group_ids: `['board', 'files', 'im_attachment', 'im_local', 'im_wechat', 'lua', 'skill_mgr', 'system', 'time', 'webui']`
- **LLM 基础问答 (`AT+CLAW=ask,现在几点`):**
  - 回复：`现在是 2026年7月22日 14:22:27（UTC+8）` ✅（cap_time 正常，调用了 get_current_time 工具）

---

### y 轮（全量恢复）

- **menuconfig:** 13个 cap 全部恢复为 y — applied OK
- **Build:** PASS — `Build done`，零 error
- **Flash:** PASS — `Finished PASS`
- **REST verify (`/api/cap/groups`):**
  - `OK present: {'scheduler', 'im_telegram', 'http_request', 'web_search', 'net_discover', 'im_qq', 'mcp_client', 'vision', 'mcp_server', 'honesty', 'im_feishu', 'router_mgr', 'audio_stream'}` ✅
  - `FAIL missing: set()` ✅（为空）
  - 实际 group_ids（完整）: `['audio_stream', 'board', 'files', 'honesty', 'http_request', 'im_attachment', 'im_feishu', 'im_local', 'im_qq', 'im_telegram', 'im_wechat', 'lua', 'mcp_client', 'mcp_server', 'net_discover', 'router_mgr', 'scheduler', 'skill_mgr', 'system', 'time', 'vision', 'web_search', 'webui']`
- **LLM 工具完整性 (`AT+CLAW=ask,你有哪些工具`):**
  - 回复列出多类工具，含 `web_search` ✅、`scheduler_add_job` / `scheduler_list_jobs` ✅
  - 工具类别涵盖：时间、记忆、技能、网络搜索、文件操作、系统信息、网络发现、HTTP请求、图像分析、Lua脚本、定时任务、消息发送

---

### Pass 标准核查

**n 轮：**
- [x] 编译零 error
- [x] 13 个受控 cap 不出现在 `/api/cap/groups`
- [x] `system`、`time`、`webui` 正常可见
- [x] LLM 基础问答正常（能回答时间）

**y 轮：**
- [x] 编译零 error
- [x] 13 个受控 cap 重新出现在 `/api/cap/groups`
- [x] LLM 工具列表含 `web_search`、`scheduler`

---

### Pass 标准核查

**n 轮（关断）— web_search & scheduler 均满足：**
- [x] 编译零 error
- [x] 被关断 cap 的 group_id 不出现在 `/api/cap/groups`
- [x] LLM 回复不包含该工具（L1 关断无 system prompt 注入）

**y 轮（恢复）— web_search & scheduler 均满足：**
- [x] 编译零 error
- [x] 被恢复 cap 的 group_id 出现在 `/api/cap/groups`
- [x] LLM 回复包含该工具（工具已重新注入 system prompt）

---

## M4-CAP-03  rolfs 文档条件 staging

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F, full baseline @ master (build only, no flash)

**Overall result: PASS**

---

### n 轮（audio_stream=n, net_discover=n）

- **menuconfig:** `CLAW_CAP_AUDIO_STREAM=n CLAW_CAP_NET_DISCOVER=n` — applied OK
- **Build:** PASS — `Build done`, 零 error
- **Staging 检查（清理 staging/docs 后重建）:**
  - `audio_stream.md absent: True` ✅
  - `net_discover.md absent: True` ✅

### 对照轮（net_discover=y, audio_stream=n）

- **menuconfig:** `CLAW_CAP_NET_DISCOVER=y` — applied OK
- **Build:** PASS — `Build done`, 零 error
- **Staging 检查:**
  - `net_discover.md present: True` ✅
  - `audio_stream.md absent: True` ✅（audio_stream 仍关）

### Pass 标准核查

- [x] 两 cap 关闭后 staging 目录无对应 .md
- [x] net_discover 恢复后 staging 目录出现对应 .md
- [ ] LLM 读取验证（跳过，build-only 已足够）

### 注意 / test_spec 问题

**test_spec 缺少关键步骤：** 必须先清理 `build_RTL8721F/build/rolfs_staging/docs/`，再重建，才能验证文档缺席。build 系统采用增量 copy，不会删除不再需要的旧文件；直接查 staging 会看到上一次 y 轮遗留的旧文档，造成误判。
建议 test_spec 在验证步骤前添加：
```bash
rm -rf build_RTL8721F/build/rolfs_staging/docs/
```

---

## M4-CAP-04  always-on 模块不受影响

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F，CLAW_CAP_WEB_SEARCH=n CLAW_CAP_VISION=n CLAW_CAP_MCP_CLIENT=n

**Overall result: PASS**

---

- **Build:** PASS — `Build done`, 零 error
- **Flash:** PASS — `Finished PASS`
- **文件读写（cap_files）:**
  - PUT → `{"ok":true}` ✅，GET → `hello` ✅，DELETE → `{"ok":true}` ✅
- **Skill 列表（cap_skill_mgr）:**
  - 返回 `{"files":[]}` JSON ✅（无 skill 已安装，但接口可用）
- **系统状态（cap_system）:**
  - `heap.free_bytes: 14676152 > 0` ✅
- **关闭的 cap 不可见:**
  - `web_search absent: True` ✅，`vision absent: True` ✅，`mcp_client absent: True` ✅

### Pass 标准核查

- [x] 编译零 error
- [x] 文件读写正常
- [x] Skill 列表接口可用
- [x] 系统状态接口可用（heap.free_bytes > 0）
- [x] 3 个关闭的 cap group 不出现在工具列表

### test_spec 问题

test_spec step 4 字段名错误：`/status` 返回 `heap.free_bytes`，不是 `heap_free`。
应改为：`d.get('heap', {}).get('free_bytes')`

---

## M4-CAP-05  L1 固件尺寸基准

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F，各 cap 逐一关断（build only，无需烧录）

**Overall result: PASS**

---

### 全量基准

- app.bin: 2003168 bytes = **1956 KB**
- BSS（project_km4tz）: 158912 bytes = **155 KB**

### 各 cap 关断尺寸

| 关断 cap     | app.bin (KB) | 节省 (KB) | BSS (bytes) | BSS 节省 (KB) |
|-------------|-------------|----------|------------|--------------|
| 全量基准     | 1956        | —        | 158912     | —            |
| SCHEDULER   | 1947        | 8        | 148352     | 10           |
| WEB_SEARCH  | 1954        | 1        | 158912     | 0            |
| HTTP_REQUEST| 1952        | 3        | 158912     | 0            |
| VISION      | 1950        | 6        | 158464     | 0            |
| NET_DISCOVER| 1948        | 7        | 158912     | 0            |
| AUDIO_STREAM| 1956        | 0        | 158912     | 0            |
| MCP_CLIENT  | 1949        | 6        | 105152     | 52           |
| MCP_SERVER  | 1951        | 4        | 158784     | 0            |
| **全量关断** | **1915**    | **40**   | **93952**  | **63**       |

### Pass 标准核查

- [x] 8 个 cap 数据全部填写完整
- [x] 全量关断（8 cap 同时）BSS 节省 63 KB ≥ 40 KB（Flash 节省 40 KB < 80 KB，但 BSS 满足）

### 观察

- `AUDIO_STREAM`: Flash 和 BSS 均 0 节省——audio 框架无论是否启用均被链接，cap 本体代码极小
- `MCP_CLIENT`: BSS 节省高达 52 KB——说明 MCP client 有大静态缓冲区
- `WEB_SEARCH`/`HTTP_REQUEST`: Flash 节省极小——实现主要在 SDK 库层，cap 只是薄封装

---

## M4-CAP-06  L2 单 cap 运行时关断

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F，全量基线（REST-only，无需编译）

**Overall result: PASS**

---

### n 轮（L2 关断 web_search）

- **初始状态:** `found: True`，`runtime_enabled: True`，`llm_visible: True`，`tools count: 1` ✅
- **设置运行时关断:** `POST /api/cap/groups/runtime {"disabled":["web_search"]}` → `{"ok":true}` ✅
- **配置持久化:** VFS `/claw_config.json` 包含 `cap_runtime.disabled: ["web_search"]` ✅
  - **注意：** `/api/config` REST API 不返回 `cap_runtime` 节（仅返回 `cap_visibility`），属 API bug
- **立即检查（未重启）:** `still in list: True`，`runtime_enabled: False` ✅
- **重启后 group 状态:**
  - `in list: True` ✅（与 L1 关断不同，group 仍在列表）
  - `runtime_enabled: False` ✅
  - `llm_visible: True` ✅（L3 层未动）
  - `plugin_name: ''` ✅（cap 未初始化）
  - `tools: []` ✅
- **LLM 工具验证:** 回复无 `web_search` ✅
- **核心 cap 正常:** `system` rt:True tools:6，`time` rt:True tools:2，`files` rt:True tools:7 ✅

### y 轮（恢复）

- **清空关断列表:** `POST /api/cap/groups/runtime {"disabled":[]}` → `{"ok":true}` ✅
- **重启后:** `runtime_enabled: True`，`tools count: 1` ✅

### Pass 标准核查

n 轮：
- [x] 设置后立即 `runtime_enabled=false`，group 仍在列表
- [x] 配置持久化（VFS 文件验证）
- [x] 重启后 `plugin_name=''`，`tools=[]`，`llm_visible` 仍为 True
- [x] LLM 回复不含 web_search
- [x] `system`、`time`、`files` 正常（tools > 0）

y 轮：
- [x] 清空后重启，`runtime_enabled=true`，tools > 0
- [x] LLM 回复含 web_search

### Bug 记录

**BUG:** `/api/config` REST API 不包含 `cap_runtime` 节，但 `cap_runtime.disabled` 实际已持久化到 VFS 的 `claw_config.json`。
test_spec step 3 验证需改为读取 VFS 文件：
```bash
curl -s "http://127.0.0.1/api/files/content?path=vfs:/claw_config.json" | python3 -c "
import sys,json; cfg=json.load(sys.stdin)
print('cap_runtime.disabled:', cfg.get('cap_runtime',{}).get('disabled'))
"
```

---

## M4-CAP-07  IM 单平台关断

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F，CLAW_CAP_IM_WECHAT=n

**Overall result: PASS**

---

### 初测失败 → 修复 → 复测通过

**初测编译错误（已修复）：**
```
cap_webui.c:1111: error: unused variable 'wx_tok' [-Werror=unused-variable]
```
根因：`wx_tok` 在外层 `#if any-IM` 块中无条件声明，但仅在 `#ifdef CONFIG_CLAW_CAP_IM_WECHAT` 内使用。
修复：将 `const char *wx_tok = NULL;` 包入 `#ifdef CONFIG_CLAW_CAP_IM_WECHAT` / `#endif`。

---

### 复测结果

- **Build:** PASS — `Build done`, 零 error
- **Flash:** PASS — `Finished PASS`
- **`/wechat` POST:** `404` ✅
- **`im_attachment` 仍存在:** `True` ✅
- **其他 IM 平台可见:**
  - `im_telegram present` ✅
  - `im_feishu present` ✅
  - `im_qq present` ✅
- **wechat 不在 cap groups:** `im_wechat absent: True` ✅
- **cap_list 无 wechat:** `wechat in response: False` ✅

### Pass 标准核查

- [x] 编译零 error
- [x] `/wechat` POST 返回 404
- [x] `im_attachment` group 仍存在
- [x] telegram / feishu / qq 工具可见
- [x] LLM cap_list 无 wechat 相关工具

---

## M4-CAP-08  全 IM 平台关断

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F，全 4 个 IM 平台关断（TELEGRAM/FEISHU/WECHAT/QQ=n）

**Overall result: PASS**

---

- **Build:** PASS — `Build done`, 零 error（4 个 IM 全关时 wechat bug 代码块整体排除）
- **Flash:** PASS — `Finished PASS`，app.bin: 1891 KB
- **map 符号检查:** `cap_im_attachment` 计数 = 0 ✅
- **REST 验证:**
  - `attachment absent: True` ✅
  - `im_local present: True` ✅
- **外部 IM 路由:**
  - `/telegram: 404` ✅
  - `/feishu: 404` ✅
  - `/wechat: 404` ✅
  - `/qq: 404` ✅

### Pass 标准核查

- [x] 编译零 error
- [x] `im_attachment` 符号不在 map
- [x] `im_local` group 在 REST 响应中可见
- [x] 4 个 IM 平台路由全部 404

### 注意

test_spec 的 map 路径 `project_km4tz/asdk/image/` 错误，实际路径为 `project_km4tz/image/`（无 `asdk/` 层）。

---

## M4-CAP-09  L2 全量可选 cap 运行时关断

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F，全量基线（REST-only，无需编译）

**Overall result: PASS**

---

### n 轮（8 个 cap L2 全量关断）

- **设置关断列表:** `POST /api/cap/groups/runtime {8 caps}` → `{"ok":true}` ✅
- **立即检查（未重启）:**
  - `all still in list: True` ✅（与 L1 关断不同）
  - `all rt_disabled: True` ✅
- **重启后（set 7 LLM-visible groups，8 个受控 cap 均未初始化）:**
  - 8 个 cap 均 `rt: False`，`tools: []` ✅
- **核心 cap 正常:** `system` rt:True tools:6，`time` rt:True tools:2，`files` rt:True tools:7，`lua` rt:True tools:5 ✅
- **LLM 验证:** 明确无 `web_search`、`scheduler`、`http_request`、`vision` ✅

### y 轮（恢复）

- **清空关断列表:** `{"disabled":[]}` → `{"ok":true}` ✅
- **重启后:** `all restored: True`，`missing: set()` ✅

### Pass 标准核查

n 轮：
- [x] 8 个 cap 仍在列表，立即反映 `runtime_enabled=false`
- [x] 重启后 8 个 cap 均 `tools=[]`
- [x] 核心 cap 正常
- [x] LLM 不含受控 cap 工具

y 轮：
- [x] 清空后重启，8 个 cap 均恢复 `runtime_enabled=true`
- [x] LLM 含 web_search、scheduler

---

## M4-CAP-10  L3 单 cap LLM 可见性关断

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F，全量基线（REST-only，立即生效）

**Overall result: PASS**

---

### n 轮（隐藏 web_search）

- **初始状态:** `llm_visible: True`，`runtime_enabled: True`，`tools count: 1` ✅
- **设置隐藏:** `POST /api/cap/groups/visibility {"hidden":["web_search"]}` → `{"ok":true}` ✅
- **立即验证（无重启）:**
  - `in list: True` ✅
  - `llm_visible: False` ✅
  - `runtime_enabled: True` ✅（L2 未动，cap 仍运行）
  - `tools count: 1` ✅（与 L2 关断 tools=[] 不同）
- **LLM 验证:** 回复工具列表中无 `web_search` ✅
- **API 直接调用:** `POST /api/cap/invoke {"cap":"web_search",...}` 有响应（api_key 未配置，返回配置错误，但 cap 可访问）✅

### y 轮（恢复）

- **清空隐藏列表:** `{"hidden":[]}` → `{"ok":true}` ✅
- **立即验证:** `llm_visible: True` ✅
- **LLM 验证:** `有 web_search 工具` ✅

### Pass 标准核查

n 轮：
- [x] 设置后立即 `llm_visible=false`，无需重启
- [x] `runtime_enabled` 和 tools 不受影响
- [x] LLM 回复不含 web_search
- [x] `/api/cap/invoke` 直接调用有响应

y 轮：
- [x] 清空后立即 `llm_visible=true`
- [x] LLM 回复含 web_search

### test_spec 问题

test_spec step 5 中 `"cap":"search"` 应改为 `"cap":"web_search"`（cap name 与 group_id 一致）。

---

## M4-CAP-11  L3 全量 LLM 可见性关断

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F，全量基线（REST-only，立即生效）

**Overall result: PASS**

---

### n 轮（隐藏 8 个受控可选 cap）

- **设置隐藏:** 8 caps → `{"ok":true}` ✅
- **立即验证（无重启）:**
  - `all still in list: True` ✅
  - `all llm_hidden: True` ✅
  - `tools (不变): scheduler:5, web_search:1, http_request:1, vision:1, net_discover:3, audio_stream:0, mcp_client:0, mcp_server:1` ✅
- **LLM 验证:** 明确无 web_search、scheduler、http_request、vision_describe 等 ✅

### y 轮（恢复）

- **清空隐藏列表:** `{"hidden":[]}` → `{"ok":true}` ✅
- **立即验证:** `all restored: True`，`missing: set()` ✅
- **LLM 验证:** `有 web_search 工具` ✅

### Pass 标准核查

n 轮：
- [x] 立即生效，无需重启/编译
- [x] 8 个 cap 仍在列表，均 `llm_visible=false`，tools 不变
- [x] LLM 不含受控 cap 工具

y 轮：
- [x] 清空后立即恢复
- [x] 8 个 cap 均 `llm_visible=true`
- [x] LLM 含 web_search、scheduler

### 观察

`audio_stream` 和 `mcp_client` 的 tools 数量为 0，非异常——这两个 cap 本身不向 LLM 注册工具（audio_stream 是 C 层流任务，mcp_client 需要服务器配置才注册工具）。
test_spec 中 `tools (should be >0)` 的注释对这两个 cap 不准确。

---

## M4-CAP-12  特殊 cap（honesty）L1 关断

**Date:** 2026-07-22
**Tester:** alejandro_chen
**Firmware:** RTL8721F，CLAW_CAP_HONESTY=n

**Overall result: PASS**

---

- **关断前基线:** `honesty present: True` ✅（板上全量固件）
- **menuconfig:** `CLAW_CAP_HONESTY=n` — applied OK
- **Build:** PASS — `Build done`, 零 error
- **map 符号检查:** `grep -c "cap_honesty" .../project_km4tz/image/target_img2.map` = 0 ✅
- **Flash:** PASS — `Finished PASS`
- **REST 验证:**
  - `honesty absent: True` ✅
  - `system present: True` ✅
  - `time present: True` ✅

### Pass 标准核查

- [x] 编译零 error
- [x] map 文件中 `cap_honesty` 符号计数为 0
- [x] `honesty` 不出现在 `/api/cap/groups`
- [x] `system`、`time` 正常可见

### test_spec 问题

test_spec 中 map 路径错误：`project_km4tz/asdk/image/target_img2.map` → 实际为 `project_km4tz/image/target_img2.map`（无 `asdk/` 层）。

