# 用例规范约定

## 编号

`M<n>-<CAT>-<NN>`：
- `M<n>` 里程碑：M1 驱动地基 / M2 上层模块 / M3 高ROI模块 / M4 工程化验收。
- `<CAT>` 分类（对齐 test_case_list.md 现有）：`LCD` `BTN` `APP` `NET` `SES` `TCH`
  `DSP` `GAME` `AUD` `SEN` `CTL` `DEMO` `ENG`。没有合适分类时可新增，但先看现有能否复用。
- `<NN>` 两位序号，取该分类下一个可用值（查 test_case_list.md）。

## 目录 / 文件命名

- 文件夹：`M<n>/<编号>_<中文用例名>`，例如 `M3/M3-SEN-02_温湿度传感器`。
- 编号、文件夹名、test_case_list.md 里的行、（若有）SKILL.md 的 `name` 字段——保持一致。
- SKILL.md 的 `name` 用英文小写下划线（= 设备上 `vfs:/skills/<name>/` 目录名），如
  `public_ip_info`、`bilibili_user_stat`。

## 等级定义（写进 Pass 判定）

| 等级 | 含义 |
|---|---|
| **L1** | 连测 3 次全过（3/3）且 iter ≤ 5（外设复杂 App 可放宽到 ≤ 6）|
| **L2** | 能跑但不稳定，或 iter 超标 |
| **L3** | 未验证 |

## VQA 占位符

- 命令一律写 `<VQA_SCRIPT>`（如 `<VQA_SCRIPT> "prompt" 标签`、`<VQA_SCRIPT> --video 6 "prompt" 标签`）。
- 在 test_spec 前置条件里加一行提醒把 `<VQA_SCRIPT>` 换成本机 `bash /path/to/auto_perf/vqa.sh`。
- 拍照 = 静态显示；`--video` = 动态/时序（秒数刷新、动画、残影、滚动）。防脑补 prompt：
  "逐行照抄 / 乱码标[乱码] / 不推断"。注入或切页后等 1s 画面稳定再拍。

## GPIO 注入约定（外设 + TESTER 板）

- 命令发给 **TESTER** 板：`AT+CLAW=gpio_ctrl,PA_xx,click|double|long,<ms>|bounce|release`。
- TESTER 与 DUT **同名引脚接线、必须共地、开漏（只拉低不拉高）**。接线细节写进 `BOARD.md`。
- 具体 pin 映射由用例决定；先让 claw 汇报它绑的按键，再据此注入。

## SKILL.md（设备 runtime skill，可选）

仅当测试需要设备预装一份**供 claw 自主发现**的能力文档/脚本时才建。结构参考
`M1-NET-01/SKILL.md`：
- YAML frontmatter：`name`（= 目录名）、`description`、`compatibility: RTL8721F`、
  `metadata: {manage_mode: runtime, category: ...}`。
- 正文：API Reference / Prerequisites（allowlist 等）/ How to invoke / Notes。
- **不是**用来放任务 prompt 的——任务 prompt 只在 test_spec.md 的 Step 2。

## scripts/（可选）

参考 Lua 脚本，作为 runtime skill 的一部分安装，或作为"直接跑某脚本验证驱动"的目标。
**绝不粘进任务 prompt**（违反第一铁律）。

## 注册到 test_case_list.md

在对应里程碑分类表加一行。不同分类表列头略有差异，对齐相邻行即可。常见列：

`| 编号 | 用例名称 | 验证目标/功能描述 | 依赖/操作 | 通过标准 | 通过 | 通过日期 |`

- "通过标准"写简版（详版在 test_spec.md），如 `3/3 返回正确数值；iter ≤ 4`。
- "通过""通过日期"新用例填 `—`。
- 更新底部"统计汇总"表：对应阶段用例数 +1，说明里的分项计数同步。

## 老用例结构对照（勿沿用旧写法）

早期 M1-APP-02/03 用 `SKILL.md`（塞 prompt）+ `Standards.md`。**新用例不要这么写**：
prompt 放 `test_spec.md` 的 Step 2，验收标准也并进 `test_spec.md`，`SKILL.md` 只保留其
"设备 runtime skill"本义。若用户明确要求规范化某个老用例，把 `Standards.md` 内容并入
`test_spec.md`、把 `SKILL.md` 里的 prompt 移到 Step 2 后按需删除或改回 runtime skill 用途。
