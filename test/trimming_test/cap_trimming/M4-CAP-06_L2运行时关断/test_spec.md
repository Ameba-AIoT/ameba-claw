# M4-CAP-06 L2 单 cap 运行时关断

验证通过 REST API 设置运行时关断后：group 仍在列表（`runtime_enabled=false`），
重启后 cap 未初始化（tools 为空），LLM 无该工具；恢复后完全还原。

三层区别一览：

- L1（Kconfig 关断）：group **消失**于 `/api/cap/groups`，需重新编译烧录
- L2（运行时关断）：group **仍在**列表，`runtime_enabled=false`，`tools=[]`，重启生效
- L3（LLM 可见性）：group 仍在列表，`runtime_enabled=true`，`tools` 有内容，仅 LLM 看不到

> L2 关断不影响 `llm_visible` 字段（L3 独立层），但 cap 未初始化，实际无工具注册，LLM 也用不了。

测试对象：`web_search`。

---

## 前置条件

清空 L2 运行时关断列表和 L3 可见性隐藏列表，重启板子，等联网（≥30 s）。

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/runtime \
  -H "Content-Type: application/json" \
  -d '{"disabled":[]}'

curl -s -X POST http://127.0.0.1/api/cap/groups/visibility \
  -H "Content-Type: application/json" \
  -d '{"hidden":[]}'
```

---

## n 轮：L2 关断

### 1. 确认初始状态

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
g = next((x for x in json.load(sys.stdin) if x['group_id']=='web_search'), None)
print('found:', g is not None)
print('runtime_enabled:', g['runtime_enabled'] if g else 'N/A')
print('llm_visible:', g['llm_visible'] if g else 'N/A')
print('tools count:', len(g.get('tools',[])) if g else 'N/A')
"
```

期望：`found: True`，`runtime_enabled: True`，`llm_visible: True`，tools count > 0。

### 2. 设置运行时关断

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/runtime \
  -H "Content-Type: application/json" \
  -d '{"disabled":["web_search"]}' | python3 -m json.tool
```

期望：`{"ok": true}`。

### 3. 验证配置持久化

注：`/api/config` REST API 不返回 `cap_runtime` 节（已知 bug），需直接读取 VFS 配置文件验证：

```bash
curl -s "http://127.0.0.1/api/files/content?path=vfs:/claw_config.json" | python3 -c "
import sys, json
cfg = json.load(sys.stdin)
print('cap_runtime.disabled:', cfg.get('cap_runtime', {}).get('disabled'))
"
```

期望：`['web_search']`。

### 4. 立即检查（未重启）

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
g = next((x for x in json.load(sys.stdin) if x['group_id']=='web_search'), None)
print('still in list:', g is not None)
print('runtime_enabled:', g['runtime_enabled'] if g else 'N/A')
"
```

期望：`still in list: True`，`runtime_enabled: False`（立即反映，无需重启）。

### 5. 重启

```bash
lock python ~/tools/at_cmd.py "reboot" -t 5
```

等待 ≥ 30 s 网络恢复。

### 6. 重启后 group 状态（核心验证）

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
g = next((x for x in json.load(sys.stdin) if x['group_id']=='web_search'), None)
print('in list:', g is not None)
print('runtime_enabled:', g.get('runtime_enabled') if g else 'N/A')
print('llm_visible:', g.get('llm_visible') if g else 'N/A')
print('plugin_name:', repr(g.get('plugin_name')) if g else 'N/A')
print('tools:', g.get('tools') if g else 'N/A')
"
```

期望：
- `in list: True`（与 L1 关断不同，group 仍出现，便于 WebUI 重新开启）
- `runtime_enabled: False`
- `llm_visible: True`（L3 层未动，config 未变）
- `plugin_name: ''`（cap 未初始化，无活跃注册）
- `tools: []`（无工具，与 L3 关断 tools 非空不同）

### 7. LLM 工具无 web_search

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你有哪些工具" -t 30 -m "CLAW_RESP"
```

期望：回复中**不含** `web_search`（cap 未初始化，工具未注册到 LLM catalog）。

### 8. 其他 cap 正常

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
groups = {g['group_id']: g for g in json.load(sys.stdin)}
for gid in ['system', 'time', 'files']:
    g = groups.get(gid, {})
    print(gid, 'rt:', g.get('runtime_enabled'), 'tools:', len(g.get('tools',[])))
"
```

期望：三个 cap 均 `runtime_enabled: True`，tools > 0。

---

## y 轮：恢复

### 9. 清空运行时关断列表

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/runtime \
  -H "Content-Type: application/json" \
  -d '{"disabled":[]}' | python3 -m json.tool
```

期望：`{"ok": true}`。

### 10. 重启

```bash
lock python ~/tools/at_cmd.py "reboot" -t 5
```

等待 ≥ 30 s。

### 11. 确认恢复

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
g = next((x for x in json.load(sys.stdin) if x['group_id']=='web_search'), None)
print('in list:', g is not None)
print('runtime_enabled:', g.get('runtime_enabled') if g else 'N/A')
print('tools count:', len(g.get('tools',[])) if g else 'N/A')
"
```

期望：`runtime_enabled: True`，tools count > 0。

### 12. LLM 工具恢复

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你有哪些工具" -t 30 -m "CLAW_RESP"
```

期望：回复中**含** `web_search`。

---

## Pass 标准

n 轮：
- [ ] 设置后立即 `runtime_enabled=false`，group 仍在列表
- [ ] 配置持久化到 `cap_runtime.disabled: ['web_search']`
- [ ] 重启后 `plugin_name=''`，`tools=[]`，`llm_visible` 仍为 True
- [ ] LLM 回复不含 web_search
- [ ] `system`、`time`、`files` 正常（tools > 0）

y 轮：
- [ ] 清空后重启，`runtime_enabled=true`，tools > 0
- [ ] LLM 回复含 web_search
