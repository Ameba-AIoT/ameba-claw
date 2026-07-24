# M4-LUA-02 Lua 模块运行时关断（disabled_modules）

验证通过 REST API 设置 `disabled_modules` 后：**立即生效，无需重启/编译**；
模块仍出现在 `modules` 数组中（`enabled=false`），`require()` 失败；
锁定模块（sys/event/cap/wifi/udp）不可关断；恢复后模块重新可用。

与 M4-LUA-01（Kconfig L1）的区别：
- L1 关断：模块**不出现**在 `modules` 数组，需重新编译烧录
- L2 运行时关断：模块**仍在**数组但 `enabled=false`，REST API 设置立即生效

---

## 单模块 n 轮：关断 gpio

### 1. 确认初始状态

```bash
curl -s http://127.0.0.1/api/lua/modules | python3 -c "
import sys, json
data = json.load(sys.stdin)
m = next((x for x in data['modules'] if x['id']=='gpio'), None)
print('gpio found:', m is not None)
print('gpio enabled:', m['enabled'] if m else 'N/A')
print('gpio locked:', m.get('locked') if m else 'N/A')
print('current disabled:', repr(data.get('disabled','')))
"
```

期望：`gpio found: True`，`gpio enabled: True`，`disabled: ''`。

### 2. 关断 gpio

```bash
curl -s -X POST http://127.0.0.1/api/lua/modules \
  -H "Content-Type: application/json" \
  -d '{"disabled":"gpio"}' | python3 -m json.tool
```

期望：`{"ok": true}`。

### 3. 立即验证（无需重启）

```bash
curl -s http://127.0.0.1/api/lua/modules | python3 -c "
import sys, json
data = json.load(sys.stdin)
m = next((x for x in data['modules'] if x['id']=='gpio'), None)
print('still in list:', m is not None)
print('gpio enabled:', m['enabled'] if m else 'N/A')
print('disabled csv:', repr(data.get('disabled','')))
"
```

期望：`still in list: True`（与 L1 不同，模块仍在列表），`gpio enabled: False`，`disabled csv: 'gpio'`。

### 4. require() 运行时失败

```bash
curl -s -X PUT "http://127.0.0.1/api/files/content?path=/scripts/test_mod.lua" \
  --data 'function run(args) local ok, err = pcall(require, args.mod); return ok and "LOADED" or ("BLOCKED:"..tostring(err)) end'

curl -s -X POST http://127.0.0.1/api/cap/invoke \
  -H "Content-Type: application/json" \
  -d '{"cap":"lua_run","input":{"path":"vfs:/scripts/test_mod.lua","args":{"mod":"gpio"}}}' \
  | cat

curl -s -X DELETE "http://127.0.0.1/api/files?path=vfs:/scripts/test_mod.lua"
```

期望：输出含 `BLOCKED`。

---

## 单模块 y 轮：恢复 gpio

### 5. 清空 disabled

```bash
curl -s -X POST http://127.0.0.1/api/lua/modules \
  -H "Content-Type: application/json" \
  -d '{"disabled":""}' | python3 -m json.tool
```

期望：`{"ok": true}`。

### 6. 立即验证（无需重启）

```bash
curl -s http://127.0.0.1/api/lua/modules | python3 -c "
import sys, json
data = json.load(sys.stdin)
m = next((x for x in data['modules'] if x['id']=='gpio'), None)
print('gpio enabled:', m['enabled'] if m else 'N/A')
print('disabled csv:', repr(data.get('disabled','')))
"
```

期望：`gpio enabled: True`，`disabled csv: ''`。

---

## 锁定模块保护验证

### 7. 尝试关断 sys（锁定模块）

```bash
curl -s -X POST http://127.0.0.1/api/lua/modules \
  -H "Content-Type: application/json" \
  -d '{"disabled":"sys"}' | python3 -m json.tool

curl -s http://127.0.0.1/api/lua/modules | python3 -c "
import sys, json
data = json.load(sys.stdin)
m = next((x for x in data['modules'] if x['id']=='sys'), None)
print('sys enabled:', m['enabled'] if m else 'N/A')
print('sys locked:', m.get('locked') if m else 'N/A')
"
```

期望：`sys enabled: True`（锁定模块不受 disabled_csv 影响），`sys locked: True`。

### 8. 恢复（清空可能残留的 disabled）

```bash
curl -s -X POST http://127.0.0.1/api/lua/modules \
  -H "Content-Type: application/json" \
  -d '{"disabled":""}' | python3 -m json.tool
```

---

## 全量：关断所有非锁定模块

### 9. 查询所有非锁定模块

```bash
curl -s http://127.0.0.1/api/lua/modules | python3 -c "
import sys, json
data = json.load(sys.stdin)
unlocked = [m['id'] for m in data['modules'] if not m.get('locked') and m.get('chip_ok', True)]
print('unlocked:', unlocked)
print('csv:', ','.join(unlocked))
"
```

### 10. 全量关断（用上一步输出的 CSV）

```bash
curl -s -X POST http://127.0.0.1/api/lua/modules \
  -H "Content-Type: application/json" \
  -d '{"disabled":"<CSV_FROM_STEP_9>"}' | python3 -m json.tool
```

### 11. 验证：非锁定全部 disabled，锁定模块仍 enabled

```bash
curl -s http://127.0.0.1/api/lua/modules | python3 -c "
import sys, json
data = json.load(sys.stdin)
locked_on   = [m['id'] for m in data['modules'] if m.get('locked') and m['enabled']]
unlocked_on = [m['id'] for m in data['modules'] if not m.get('locked') and m['enabled']]
print('locked still on:', locked_on)
print('unlocked still on (should be []):', unlocked_on)
"
```

期望：`locked still on` 非空（sys 等锁定模块仍 enabled），`unlocked still on: []`。

### 12. 全量恢复

```bash
curl -s -X POST http://127.0.0.1/api/lua/modules \
  -H "Content-Type: application/json" \
  -d '{"disabled":""}' | python3 -m json.tool
```

---

## Pass 标准

单模块 n 轮：
- [ ] **立即生效**，无需重启/编译
- [ ] 模块仍在 `modules` 数组，`enabled=false`（与 L1 关断不在列表不同）
- [ ] require() 输出含 `BLOCKED`

单模块 y 轮：
- [ ] 清空后**立即** `enabled=true`

锁定模块：
- [ ] `sys` disabled 后仍 `enabled=true`，`locked=true`

全量：
- [ ] 非锁定模块全部 `enabled=false`
- [ ] 锁定模块仍 `enabled=true`
