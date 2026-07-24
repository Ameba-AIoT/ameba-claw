# M4-CAP-08 全 IM 平台关断

关闭全部 4 个 IM 平台后，`cap_im_attachment` 随之关断；`cap_im_local` 始终保留。

## 配置

```bash
./ameba.py menuconfig --set \
  CLAW_CAP_IM_TELEGRAM=n \
  CLAW_CAP_IM_FEISHU=n \
  CLAW_CAP_IM_WECHAT=n \
  CLAW_CAP_IM_QQ=n \
  CLAW_CAP_IM_ATTACHMENT=n
```

## 验证步骤

**1. 编译 — 确认 attachment 符号不在 map**
```bash
./ameba.py build -a . 2>&1 | grep -E "error:|Build done|FAIL"
grep -c "cap_im_attachment" build_RTL8721F/build/project_km4tz/image/target_img2.map 2>/dev/null || echo "0"
```
期望：`Build done`；map 中计数为 0。

**2. 烧录 + 等联网**

**3. attachment 消失，im_local 保留**
```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "
import sys, json
ids = {g['group_id'] for g in json.load(sys.stdin)}
print('attachment absent:', not any('attachment' in i for i in ids))
print('im_local present:', any('local' in i for i in ids))
"
```
期望：两者均为 `True`。

**4. 外部 IM 路由全部 404**
```bash
for path in /telegram /feishu /wechat /qq; do
  echo -n "$path: "
  curl -s -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1$path"
  echo
done
```
期望：全部 `404`。

## 恢复

```bash
./ameba.py menuconfig --set \
  CLAW_CAP_IM_TELEGRAM=y \
  CLAW_CAP_IM_FEISHU=y \
  CLAW_CAP_IM_WECHAT=y \
  CLAW_CAP_IM_QQ=y \
  CLAW_CAP_IM_ATTACHMENT=y
```

## Pass 标准

- [ ] 编译零 error
- [ ] `im_attachment` 符号不在 map
- [ ] `im_local` group 在 REST 响应中可见
- [ ] 4 个 IM 平台路由全部 404
