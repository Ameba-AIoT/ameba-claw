# M4-CAP-01 L1 单 cap 关断（Kconfig）

验证通过 Kconfig 关闭/开启单个可选 cap 后：编译干净，对应 group 在 `/api/cap/groups` 中**完全消失/出现**，且 LLM 工具列表同步变化。

> 三层关断中的 **第一层**：代码不编译进固件，Flash 占用减少，需重新烧录。
> 与 L2（运行时关断，见 M4-CAP-06）的区别：L1 关断的 cap group 完全从列表消失；L2 关断的 cap group 仍出现在列表（`runtime_enabled=false`）。

---

## 测试矩阵（代表性 2 个 cap）

| # | Kconfig 符号 | group_id | LLM 询问语句 | 验证方式 |
|---|---|---|---|---|
| 1 | `CLAW_CAP_WEB_SEARCH` | `web_search` | `你是否有web_search工具` | REST + LLM |
| 2 | `CLAW_CAP_SCHEDULER` | `scheduler` | `你是否有scheduler工具` | REST + LLM |

两个 cap 覆盖典型场景：
- `web_search`：无依赖的简单可选 cap，最小干扰
- `scheduler`：带 IO 阶段（HTTP 路由 + wifi hook）的可选 cap，验证多阶段 hook 均被裁剪

> 其他 cap 的 Kconfig 关断遵循相同模式，可按需对照此规程执行。
> 有 Kconfig 依赖的 cap（如 IM 系列依赖 WEBUI）参见 M4-CAP-07。

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

## 通用步骤（对每一条目，执行 n 轮和 y 轮各一次）

### n 轮：关断目标 cap

#### 1. 关断

```bash
./ameba.py menuconfig --set CLAW_CAP_<X>=n
```

#### 2. 编译

```bash
./ameba.py build -a . 2>&1 | grep -E "error:|Build done|FAIL"
```

期望：只有 `Build done`，无 `error:`。

#### 3. 烧录 + 等联网（≥ 30 s）

> REST API 在烧录后需等 **≥30s** 才稳定可用（cap 注册 + HTTP server 启动）；20s 可能查到不完整的 group 列表。

#### 4a. REST 验证（n）

```bash
TARGET=<group_id>
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
groups = json.load(sys.stdin)
ids = {g['group_id'] for g in groups}
print('TARGET absent :', '$TARGET' not in ids)
"
```

期望：`TARGET absent: True`。

#### 4b. LLM 验证（n）

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你是否有<group_id>工具" -t 30 -m "CLAW_RESP"
```

期望：LLM 回复中**不包含**该工具名称（明确表示没有该工具，或列举工具时不出现它）。

---

### y 轮：恢复目标 cap

#### 5. 恢复

```bash
./ameba.py menuconfig --set CLAW_CAP_<X>=y
```

#### 6. 编译

```bash
./ameba.py build -a . 2>&1 | grep -E "error:|Build done|FAIL"
```

期望：只有 `Build done`，无 `error:`。

#### 7. 烧录 + 等联网（≥ 30 s）

#### 8a. REST 验证（y）

```bash
TARGET=<group_id>
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
groups = json.load(sys.stdin)
ids = {g['group_id'] for g in groups}
print('TARGET present:', '$TARGET' in ids)
"
```

期望：`TARGET present: True`。

#### 8b. LLM 验证（y）

```bash
lock python ~/tools/at_cmd.py "AT+CLAW=ask,你是否有<group_id>工具" -t 30 -m "CLAW_RESP"
```

期望：LLM 回复中**包含**该工具名称（确认拥有该工具）。

---

## Pass 标准（每条目，n 轮 + y 轮均需通过）

**n 轮（关断）**
- [ ] 编译零 error
- [ ] 被关断 cap 的 group_id **不出现**在 `/api/cap/groups`
- [ ] LLM 回复**不包含**该工具（L1 关断无 system prompt 注入）

**y 轮（恢复）**
- [ ] 编译零 error
- [ ] 被恢复 cap 的 group_id **出现**在 `/api/cap/groups`
- [ ] LLM 回复**包含**该工具（工具已重新注入 system prompt）
