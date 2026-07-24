# M4-CAP-11 L3 全量 LLM 可见性关断

验证隐藏全部受控可选 cap 后，LLM 工具列表中这些工具消失（核心 cap 不受影响）；
恢复后 LLM 工具完整。全程**立即生效，无需重启、无需编译**。

受控可选 cap 8 个：`scheduler`、`web_search`、`http_request`、`vision`、
`net_discover`、`audio_stream`、`mcp_client`、`mcp_server`。

---

## 前置条件

清空 L2 运行时关断列表，重启板子，等联网（≥30 s）。

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/runtime \
  -H "Content-Type: application/json" \
  -d '{"disabled":[]}'
```

---

## n 轮：全量 L3 隐藏

### 1. 隐藏全部受控可选 cap

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/visibility \
  -H "Content-Type: application/json" \
  -d '{"hidden":["scheduler","web_search","http_request","vision","net_discover","audio_stream","mcp_client","mcp_server"]}' \
  | python3 -m json.tool
```

期望：`{"ok": true}`。

### 2. 立即验证（无需重启）

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
groups = json.load(sys.stdin)
removed = {'scheduler','web_search','http_request','vision','net_discover','audio_stream','mcp_client','mcp_server'}
in_list    = {g['group_id'] for g in groups}
llm_hidden = {g['group_id'] for g in groups if not g['llm_visible']}
tools_by   = {g['group_id']: len(g.get('tools',[])) for g in groups if g['group_id'] in removed}
print('all still in list:', removed <= in_list)
print('all llm_hidden:', removed <= llm_hidden)
print('tools (should be >0):', tools_by)
"
```

期望：所有 8 个 cap 仍在列表，均 `llm_visible=false`，tools 数量不变（cap 仍在运行）。

### 3. LLM 无可选工具

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你有哪些工具" -t 30 -m "CLAW_RESP"
```

期望：回复中不含 `web_search`、`scheduler`；若有工具，仅来自核心 cap（如时间查询、文件操作等）。

---

## y 轮：全量恢复

### 4. 清空隐藏列表

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/visibility \
  -H "Content-Type: application/json" \
  -d '{"hidden":[]}' | python3 -m json.tool
```

期望：`{"ok": true}`。

### 5. 立即验证（无需重启）

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
groups = json.load(sys.stdin)
expected   = {'scheduler','web_search','http_request','vision','net_discover','audio_stream','mcp_client','mcp_server'}
llm_visible = {g['group_id'] for g in groups if g['llm_visible']}
print('all restored:', expected <= llm_visible)
print('missing:', expected - llm_visible)
"
```

期望：`all restored: True`。

### 6. LLM 工具完整

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你有哪些工具" -t 30 -m "CLAW_RESP"
```

期望：回复含 `web_search`、`scheduler`。

---

## Pass 标准

n 轮：
- [ ] **立即生效**，无需重启/编译
- [ ] 8 个 cap 仍在列表，均 `llm_visible=false`，tools 不变（cap 仍运行）
- [ ] LLM 不含受控 cap 工具

y 轮：
- [ ] 清空后**立即**恢复，无需重启
- [ ] 8 个 cap 均 `llm_visible=true`
- [ ] LLM 含 web_search、scheduler
