# M4-CAP-07 IM 单平台关断

验证 `CONFIG_CLAW_CAP_IM_WECHAT=n` 后 wechat 路由 404、LLM 无 wechat 工具，其他 IM 正常。

## 配置

```bash
./ameba.py menuconfig --set CLAW_CAP_IM_WECHAT=n
```

## 验证步骤

**1. 编译**
```bash
./ameba.py build -a . 2>&1 | grep -E "error:|Build done|FAIL"
```

**2. 烧录 + 等联网**

**3. Wechat 路由消失**
```bash
curl -s -o /dev/null -w "%{http_code}" -X POST http://127.0.0.1/wechat
```
期望：`404`。

**4. IM attachment 仍存在**
```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys,json
ids = {g['group_id'] for g in json.load(sys.stdin)}
print('attachment present:', any('attachment' in i for i in ids))
"
```
期望：`True`。

**5. 其他 IM 平台正常**
```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys,json
ids = {g['group_id'] for g in json.load(sys.stdin)}
for cap in ['im_telegram','im_feishu','im_qq']:
    print(cap, 'present' if cap in ids else 'MISSING')
"
```
期望：telegram、feishu、qq 均 present。

**6. LLM 工具列表无 wechat**
```bash
curl -s -X POST http://127.0.0.1/api/cap/invoke \
  -H "Content-Type: application/json" \
  -d '{"cap":"cap_list","input":{}}' | python3 -c "import sys; print('wechat' in sys.stdin.read())"
```
期望：`False`。

## 恢复

```bash
./ameba.py menuconfig --set CLAW_CAP_IM_WECHAT=y
```

## Pass 标准

- [ ] 编译零 error
- [ ] `/wechat` POST 返回 404
- [ ] `im_attachment` group 仍存在
- [ ] telegram / feishu / qq 工具可见
- [ ] LLM cap_list 无 wechat 相关工具
