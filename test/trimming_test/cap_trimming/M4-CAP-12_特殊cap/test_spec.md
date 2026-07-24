# M4-CAP-12 特殊 cap L1 关断（honesty）

honesty 是 `tools=0` 的特殊 cap，无法通过"工具消失"来验证关断效果，
需要通过 map 文件或串口日志来确认。

honesty：AGENT-phase 观测 hook，无工具。L1 关断后：从 `/api/cap/groups` 消失，map 文件无符号。

> **注**：cap_webui 已升级为 CORE cap（与 cap_lua 同级，始终编译），不再有 Kconfig 符号，无法通过 L1 关断。

---

## 场景一：CLAW_CAP_HONESTY=n

### 1. 确认基线（关断前）

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
ids = {g['group_id'] for g in json.load(sys.stdin)}
print('honesty present:', 'honesty' in ids)
"
```

期望：`honesty present: True`。

### 2. 配置

```bash
./ameba.py menuconfig --set CLAW_CAP_HONESTY=n
```

### 3. 编译 + 符号检查（无需烧录即可验证）

```bash
./ameba.py build -a . 2>&1 | grep -E "error:|Build done|FAIL"
grep -c "cap_honesty" build_RTL8721F/build/project_km4tz/image/target_img2.map 2>/dev/null || echo "0"
```

期望：`Build done`；map 中 `cap_honesty` 符号计数为 0。

### 4. 烧录 + 等联网（≥ 30 s）

### 5. REST — honesty 消失

```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
ids = {g['group_id'] for g in json.load(sys.stdin)}
print('honesty absent:', 'honesty' not in ids)
print('system present:', 'system' in ids)
print('time   present:', 'time' in ids)
"
```

期望：`honesty absent: True`，`system present: True`，`time present: True`。

### 6. 恢复

```bash
./ameba.py menuconfig --set CLAW_CAP_HONESTY=y
```

---

## Pass 标准

honesty 场景：
- [ ] 编译零 error
- [ ] map 文件中 `cap_honesty` 符号计数为 0
- [ ] `honesty` 不出现在 `/api/cap/groups`（关断前出现，关断后消失）
- [ ] `system`、`time` 正常可见
