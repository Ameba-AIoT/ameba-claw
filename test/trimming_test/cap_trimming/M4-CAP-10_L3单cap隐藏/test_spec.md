# M4-CAP-10 L3 单 cap LLM 可见性关断

验证通过 REST API 设置 LLM 可见性关断后：**立即生效，无需重启**；
group 仍在列表，`llm_visible=false`，`tools` 仍有内容（cap 正常运行）；
LLM 工具列表中该工具消失；恢复后 LLM 重新获得该工具。

三层区别一览：

- L1（Kconfig 关断）：group 消失，tools 消失，需重新编译烧录
- L2（运行时关断）：group 仍在，`runtime_enabled=false`，`tools=[]`，需重启
- L3（LLM 可见性）：group 仍在，`runtime_enabled=true`，tools 有内容，**仅 LLM 看不到**，**立即生效**

测试对象：`web_search`。

---

## 前置条件

清空 L2 运行时关断列表，重启板子，等联网（≥30 s）。

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/runtime \
  -H "Content-Type: application/json" \
  -d '{"disabled":[]}'
```

---

## n 轮：L3 隐藏

### 1. 确认初始状态

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
g = next((x for x in json.load(sys.stdin) if x['group_id']=='web_search'), None)
print('llm_visible:', g['llm_visible'] if g else 'N/A')
print('runtime_enabled:', g['runtime_enabled'] if g else 'N/A')
print('tools count:', len(g.get('tools',[])) if g else 'N/A')
"
```

期望：`llm_visible: True`，`runtime_enabled: True`，tools count > 0。

### 2. 设置 LLM 隐藏

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/visibility \
  -H "Content-Type: application/json" \
  -d '{"hidden":["web_search"]}' | python3 -m json.tool
```

期望：`{"ok": true}`。

### 3. 立即验证（无需重启）

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
g = next((x for x in json.load(sys.stdin) if x['group_id']=='web_search'), None)
print('in list:', g is not None)
print('llm_visible:', g['llm_visible'] if g else 'N/A')
print('runtime_enabled:', g['runtime_enabled'] if g else 'N/A')
print('tools count:', len(g.get('tools',[])) if g else 'N/A')
"
```

期望：
- `in list: True`
- `llm_visible: False`（L3 关断）
- `runtime_enabled: True`（L2 未动，cap 仍运行）
- tools count > 0（与 L2 关断 tools=[] 不同）

### 4. LLM 看不到工具

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你有哪些工具" -t 30 -m "CLAW_RESP"
```

期望：回复中**不含** `web_search`。

### 5. cap 仍可通过 API 直接调用

```bash
curl -s -X POST http://127.0.0.1/api/cap/invoke \
  -H "Content-Type: application/json" \
  -d '{"cap":"web_search","input":{"query":"test"}}' \
  | python3 -c "import sys; r=sys.stdin.read(); print('got response:', len(r)>0)"
```

期望：有响应（L3 只影响 LLM 可见性，不影响 API 直接调用）。

---

## y 轮：恢复可见性

### 6. 清空隐藏列表

```bash
curl -s -X POST http://127.0.0.1/api/cap/groups/visibility \
  -H "Content-Type: application/json" \
  -d '{"hidden":[]}' | python3 -m json.tool
```

期望：`{"ok": true}`。

### 7. 立即验证（无需重启）

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
g = next((x for x in json.load(sys.stdin) if x['group_id']=='web_search'), None)
print('llm_visible:', g['llm_visible'] if g else 'N/A')
"
```

期望：`llm_visible: True`。

### 8. LLM 工具恢复

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你有哪些工具" -t 30 -m "CLAW_RESP"
```

期望：回复中**含** `web_search`。

---

## Pass 标准

n 轮：
- [ ] 设置后**立即** `llm_visible=false`，无需重启
- [ ] `runtime_enabled` 和 tools 不受影响（cap 正常运行）
- [ ] LLM 回复不含 web_search
- [ ] `/api/cap/invoke` 直接调用有响应

y 轮：
- [ ] 清空后**立即** `llm_visible=true`，无需重启
- [ ] LLM 回复含 web_search
