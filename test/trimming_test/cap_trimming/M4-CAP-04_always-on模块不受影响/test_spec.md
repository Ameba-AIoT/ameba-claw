# M4-CAP-04 always-on 模块不受影响

关闭 3 个受控 cap 后，核心 cap（`files`、`skill_mgr`、`system`）功能完整。

## 配置

```bash
./ameba.py menuconfig --set CLAW_CAP_WEB_SEARCH=n CLAW_CAP_VISION=n CLAW_CAP_MCP_CLIENT=n
```

## 验证步骤

**1. 编译烧录**
```bash
./ameba.py build -a . 2>&1 | grep -E "error:|Build done|FAIL"
```

**2. 文件读写（cap_files）**
```bash
curl -s -X PUT "http://127.0.0.1/api/files/content?path=/test_cap04.txt" --data "hello"
curl -s "http://127.0.0.1/api/files/content?path=/test_cap04.txt"
curl -s -X DELETE "http://127.0.0.1/api/files?path=vfs:/test_cap04.txt"
```
期望：PUT 返回 `{"ok":true}`，GET 返回 `hello`，DELETE 返回 `{"ok":true}`。

**3. Skill 列表（cap_skill_mgr）**
```bash
curl -s http://127.0.0.1/api/lua | python3 -m json.tool
```
期望：返回 JSON 数组（可为空），不报错。

**4. 系统状态（cap_system）**
```bash
curl -s http://127.0.0.1/status | python3 -c "import sys,json;d=json.load(sys.stdin);print('heap_free:',d.get('heap',{}).get('free_bytes'))"
```
期望：`heap_free` 有合理值（> 0）。注：`/status` 返回 `heap.free_bytes`，不是顶层 `heap_free`。

**5. 关闭的 cap 不可见**
```bash
curl -s http://127.0.0.1/api/cap/groups | python3 -c "import sys,json;[print(g['group_id']) for g in json.load(sys.stdin)]"
```
期望：无 `web_search`、`vision`、`mcp_client`。

## 恢复

```bash
./ameba.py menuconfig --set CLAW_CAP_WEB_SEARCH=y CLAW_CAP_VISION=y CLAW_CAP_MCP_CLIENT=y
```

## Pass 标准

- [ ] 编译零 error
- [ ] 文件读写正常
- [ ] Skill 列表接口可用
- [ ] 系统状态接口可用
- [ ] 3 个关闭的 cap group 不出现在工具列表
