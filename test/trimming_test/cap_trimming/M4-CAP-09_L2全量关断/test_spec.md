# M4-CAP-09 L2 全量可选 cap 运行时关断

验证通过 REST API 同时关断全部受控可选 cap 后：8 个 group 仍在列表（`runtime_enabled=false`），
重启后 LLM 只有核心 cap 工具；恢复后 LLM 工具完整。

> 无需编译/烧录，仅 REST API 操作 + 重启（与 L1 全量测试 M4-CAP-02 的核心区别）。

受控可选 cap 8 个：`scheduler`、`web_search`、`http_request`、`vision`、
`net_discover`、`audio_stream`、`mcp_client`、`mcp_server`。

---

## 前置条件

清空 L3 可见性隐藏列表（L3 变更立即生效，无需重启）。

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/visibility \
  -H "Content-Type: application/json" \
  -d '{"hidden":[]}'
```

---

## n 轮：全量 L2 关断

### 1. 设置运行时关断

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/runtime \
  -H "Content-Type: application/json" \
  -d '{"disabled":["scheduler","web_search","http_request","vision","net_discover","audio_stream","mcp_client","mcp_server"]}' \
  | python3 -m json.tool
```

期望：`{"ok": true}`。

### 2. 立即检查（未重启）

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
groups = json.load(sys.stdin)
removed = {'scheduler','web_search','http_request','vision','net_discover','audio_stream','mcp_client','mcp_server'}
in_list = {g['group_id'] for g in groups}
rt_off   = {g['group_id'] for g in groups if not g['runtime_enabled']}
print('all still in list:', removed <= in_list)
print('all rt_disabled:', removed <= rt_off)
"
```

期望：两项均 `True`（与 L1 关断不同：group 仍在列表）。

### 3. 重启

```bash
lock python ~/tools/at_cmd.py "reboot" -t 5
```

等待 ≥ 30 s 网络恢复。

### 4. 重启后：8 个 cap 状态

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
groups = json.load(sys.stdin)
removed = {'scheduler','web_search','http_request','vision','net_discover','audio_stream','mcp_client','mcp_server'}
for g in groups:
    if g['group_id'] in removed:
        print(g['group_id'], '| rt:', g['runtime_enabled'], '| tools:', g['tools'])
"
```

期望：8 个 cap 均 `rt: False`，`tools: []`。

### 5. 核心 cap 正常

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
groups = {g['group_id']: g for g in json.load(sys.stdin)}
for gid in ['system', 'time', 'files', 'lua']:
    g = groups.get(gid, {})
    print(gid, 'rt:', g.get('runtime_enabled'), 'tools:', len(g.get('tools',[])))
"
```

期望：4 个核心 cap 均 `runtime_enabled: True`，tools > 0。

### 6. LLM 工具（n 轮）

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你有哪些工具" -t 30 -m "CLAW_RESP"
```

期望：回复中不含 `web_search`、`scheduler`；含核心工具（如时间查询、get_info 等）。

---

## y 轮：全量恢复

### 7. 清空关断列表

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/runtime \
  -H "Content-Type: application/json" \
  -d '{"disabled":[]}' | python3 -m json.tool
```

期望：`{"ok": true}`。

### 8. 重启

```bash
lock python ~/tools/at_cmd.py "reboot" -t 5
```

等待 ≥ 30 s。

### 9. 重启后：8 个 cap 全部恢复

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
groups = json.load(sys.stdin)
expected = {'scheduler','web_search','http_request','vision','net_discover','audio_stream','mcp_client','mcp_server'}
rt_on = {g['group_id'] for g in groups if g['runtime_enabled']}
print('all restored:', expected <= rt_on)
print('missing:', expected - rt_on)
"
```

期望：`all restored: True`。

### 10. LLM 工具完整（y 轮）

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你有哪些工具" -t 30 -m "CLAW_RESP"
```

期望：回复含 `web_search`、`scheduler`。

---

## Pass 标准

n 轮：
- [ ] 8 个 cap 仍在 `/api/cap/groups` 列表（与 L1 全量关断的区别）
- [ ] 立即反映 `runtime_enabled=false`，无需重启
- [ ] 重启后 8 个 cap 均 `tools=[]`
- [ ] 核心 cap（system、time、files、lua）正常
- [ ] LLM 不含受控 cap 工具

y 轮：
- [ ] 清空后重启，8 个 cap 均恢复 `runtime_enabled=true`
- [ ] LLM 含 web_search、scheduler
