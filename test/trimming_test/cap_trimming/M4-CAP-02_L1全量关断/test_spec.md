# M4-CAP-02 L1 全量可选 cap 关断（Kconfig）

验证同时关闭全部受控可选 cap 后：编译干净，所有可选 group 从 `/api/cap/groups` 消失；
恢复后编译干净，所有可选 group 重新出现，LLM 工具完整。

---

## 受控可选 cap 的枚举方法

**不要硬编码 cap 列表。** 每次执行测试前，从 Kconfig 动态枚举：

```bash
# 固定保留集：核心功能 + IM 基础设施（REST 验证依赖）
KEEP="^(CLAW_CAP_TIME|CLAW_CAP_FILES|CLAW_CAP_SYSTEM|CLAW_CAP_BOARD_MGR|CLAW_CAP_SKILL_MGR|CLAW_CAP_IM_LOCAL|CLAW_CAP_IM_WECHAT|CLAW_CAP_IM_ATTACHMENT)$"

# 所有 CLAW_CAP_* 减去保留集 = 受控可选 cap
OPTIONAL=$(grep "^config CLAW_CAP_" Kconfig | sed 's/config //' | grep -vE "$KEEP")
echo "$OPTIONAL"
```

Kconfig 中新增的 `CLAW_CAP_*` 会自动纳入 OPTIONAL，无需修改本测试。
如需将某个 cap 永久保留开启（例如新增 REST 基础设施），将其加入 KEEP 即可。

**固定保留集说明：**
- 核心：`TIME`、`FILES`、`SYSTEM`、`BOARD_MGR`、`SKILL_MGR`
- IM 基础设施：`IM_LOCAL`、`IM_WECHAT`、`IM_ATTACHMENT`（REST 验证所需）

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

## n 轮：全量关断

### 1. 枚举 + 配置

```bash
KEEP="^(CLAW_CAP_TIME|CLAW_CAP_FILES|CLAW_CAP_SYSTEM|CLAW_CAP_BOARD_MGR|CLAW_CAP_SKILL_MGR|CLAW_CAP_IM_LOCAL|CLAW_CAP_IM_WECHAT|CLAW_CAP_IM_ATTACHMENT)$"
OPTIONAL=$(grep "^config CLAW_CAP_" Kconfig | sed 's/config //' | grep -vE "$KEEP")

python /home/alejandro_chen/sdk/ameba.py menuconfig --set \
  $(echo "$OPTIONAL" | sed 's/$/=n/' | xargs)
```

### 2. 编译

```bash
python /home/alejandro_chen/sdk/ameba.py build -a /home/alejandro_chen/ameba_claw 2>&1 | grep -E "error:|Build done|FAIL"
```

期望：只有 `Build done`，无 `error:`。

### 3. 烧录 + 等联网（≥ 30 s）

### 4. REST — 确认所有可选 cap 消失

```bash
KEEP="^(CLAW_CAP_TIME|CLAW_CAP_FILES|CLAW_CAP_SYSTEM|CLAW_CAP_BOARD_MGR|CLAW_CAP_SKILL_MGR|CLAW_CAP_IM_LOCAL|CLAW_CAP_IM_WECHAT|CLAW_CAP_IM_ATTACHMENT)$"
OPTIONAL_IDS=$(grep "^config CLAW_CAP_" Kconfig | sed 's/config CLAW_CAP_//' | tr 'A-Z' 'a-z' | grep -vE "^(time|files|system|board_mgr|skill_mgr|im_local|im_wechat|im_attachment)$")

curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
ids = {g['group_id'] for g in json.load(sys.stdin)}
optional = set('''$OPTIONAL_IDS'''.split())
print('OK missing:', optional - ids)
print('FAIL still present:', optional & ids)
print('system present:', 'system' in ids)
print('time   present:', 'time' in ids)
print('webui  present:', 'webui' in ids)
"
```

期望：`FAIL still present: set()` 为空，`system`、`time`、`webui` 均为 True。

### 5. LLM 基础问答（n 轮）

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,现在几点" -t 30 -m "CLAW_RESP"
```

期望：LLM 能正常回答时间（`cap_time` 仍在）。

---

## y 轮：全量恢复

### 6. 枚举 + 配置

```bash
KEEP="^(CLAW_CAP_TIME|CLAW_CAP_FILES|CLAW_CAP_SYSTEM|CLAW_CAP_BOARD_MGR|CLAW_CAP_SKILL_MGR|CLAW_CAP_IM_LOCAL|CLAW_CAP_IM_WECHAT|CLAW_CAP_IM_ATTACHMENT)$"
OPTIONAL=$(grep "^config CLAW_CAP_" Kconfig | sed 's/config //' | grep -vE "$KEEP")

python /home/alejandro_chen/sdk/ameba.py menuconfig --set \
  $(echo "$OPTIONAL" | sed 's/$/=y/' | xargs)
```

### 7. 编译

```bash
python /home/alejandro_chen/sdk/ameba.py build -a /home/alejandro_chen/ameba_claw 2>&1 | grep -E "error:|Build done|FAIL"
```

期望：只有 `Build done`，无 `error:`。

### 8. 烧录 + 等联网（≥ 30 s）

### 9. REST — 确认所有可选 cap 回来

```bash
KEEP="^(CLAW_CAP_TIME|CLAW_CAP_FILES|CLAW_CAP_SYSTEM|CLAW_CAP_BOARD_MGR|CLAW_CAP_SKILL_MGR|CLAW_CAP_IM_LOCAL|CLAW_CAP_IM_WECHAT|CLAW_CAP_IM_ATTACHMENT)$"
OPTIONAL_IDS=$(grep "^config CLAW_CAP_" Kconfig | sed 's/config CLAW_CAP_//' | tr 'A-Z' 'a-z' | grep -vE "^(time|files|system|board_mgr|skill_mgr|im_local|im_wechat|im_attachment)$")

curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
ids = {g['group_id'] for g in json.load(sys.stdin)}
optional = set('''$OPTIONAL_IDS'''.split())
print('OK present:', optional & ids)
print('FAIL missing:', optional - ids)
"
```

期望：`FAIL missing: set()` 为空。

### 10. LLM 工具完整（y 轮）

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你有哪些工具" -t 30 -m "CLAW_RESP"
```

期望：LLM 工具列表包含多个工具（含 `web_search`、`scheduler` 等可选 cap 的工具）。

---

## Pass 标准

n 轮：
- [ ] 编译零 error
- [ ] 所有受控可选 cap 不出现在 `/api/cap/groups`（`FAIL still present: set()`）
- [ ] `system`、`time`、`webui` 正常可见
- [ ] LLM 基础问答正常（能回答时间）

y 轮：
- [ ] 编译零 error
- [ ] 所有受控可选 cap 重新出现在 `/api/cap/groups`（`FAIL missing: set()`）
- [ ] LLM 工具列表包含可选 cap 的工具
