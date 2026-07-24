# Integration Tests

构建级 + 运行时集成测试。与 `rest_api_test/`（REST API 接口回归）和 `agent_auto_test/`（LLM 代理行为验证）不同，这里的测试需要特定的 **Kconfig 配置 + 重新编译 + 烧录**，不适合纳入功能层面的 harness 验收用例（`test_case_list.md`）。

## 目录

| 子目录 | 内容 |
|--------|------|
| `cap_trimming/` | Kconfig 编译可见性测试：验证关闭 cap 后编译产物、LLM 工具列表、WebUI REST 响应的正确性 |
| `lua_trimming/` | Kconfig Lua 驱动裁剪测试：验证关闭 `CLAW_LUA_DRV_*` 后驱动从模块列表消失，require 失败 |

## 运行方法

每个子目录下的 `test_spec.md` 描述具体步骤：
1. 用 `./ameba.py menuconfig --set SYMBOL=n` 设置目标 Kconfig（直接写入 `.config`，不修改 `prj.conf`）
2. 重新编译：`./ameba.py build -a .`
3. 烧录并按 spec 验证
4. 测完用 `./ameba.py menuconfig --set SYMBOL=y` 或 `menuconfig -r` 恢复
