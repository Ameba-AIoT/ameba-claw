---
name: claw-create-test-cases
description: >-
  Scaffold a standardized ameba-claw harness test case under
  ameba_claw/internal_project_ctrl/test_case_list/. Generates the test-case
  folder (test_spec.md + optional SKILL.md / scripts/ / BOARD.md) and registers
  it in test_case_list.md. Use this whenever the user wants to create, author,
  scaffold, add, or 规范/规范化 an ameba-claw test case, test spec, 测试用例, 验收用例, or
  harness test — including phrasings like "给 XXX 传感器建个测试用例", "为某功能写 test spec",
  "add a test case for the clock app", or "把这个用例补到 test_case_list". This skill
  GENERATES the test files; it does NOT run the test or talk to the device.
---

# claw-create-test-cases

生成一个**标准化的 ameba-claw harness 测试用例文件夹**，放到
`ameba_claw/internal_project_ctrl/test_case_list/`，并注册到
`test_case_list.md`。

## 这个 skill 是干什么的（以及不干什么）

我们通过跑这些测试用例来观测 ameba-claw 的 **agent harness** 表现——claw 能否
自主探索 cap/skill/文档完整跑通一个任务、结果质量如何、以及**它在过程中撞到了
哪些 harness 障碍**。收集到的障碍会反过来驱动 harness（Lua 运行方式、某个 cap、
driver、说明文档等）的自迭代。**这个 skill 的唯一职责是产出规范的测试规格文件**，
让未来可以批量、可复现地跑这些测试。

**它不做的事：**
- ❌ 不烧录、不发 `AT+CLAW`、不连串口、不实际执行测试。
- ❌ 不在生成的 prompt 里写任何 Lua 代码或 API 用法提示（见下方"第一铁律"）。

## 第一铁律：下发给 claw 的 prompt 只说"要什么"，绝不说"怎么做"

被测任务的价值，就在于观察 claw **自己**去发现能力、遇阻、绕过的全过程——这正是
harness 被测的内容。所以你为用例撰写的任务 prompt（会原样通过 `AT+CLAW=ask,...`
喂给设备上的 LLM）**必须是纯自然语言需求描述**：

- ✅ "写一个持续运行的程序，监测 PA_22/PA_23/PA_24 三个按键的单击、长按、持续按住事件，
  在 OLED 上实时显示最近的按键事件。"
- ❌ 任何 `cap.call(...)`、`require("...")`、具体函数名、字段名、pin 配置代码、
  "用 http_request 调用 xxx API" 这类提示。

如果一个用例需要设备侧预装参考资料或脚本，那些放进 `SKILL.md` / `scripts/`
（它们由 claw 自主发现、按需读取），**永远不要**把它们的内容粘进任务 prompt。

## 工作流程

### 1. 收集用例信息（不清楚就问用户）

- **验证目标**：这个用例要压测 harness 的哪个方面 / 哪个 cap / driver / 外设？
- **编号与里程碑**：`M<n>-<CAT>-<NN>`（如 `M3-SEN-02`）。看 `test_case_list.md`
  现有分类（LCD/BTN/APP/NET/SES/DSP/GAME/AUD/SEN/CTL/DEMO/ENG…）选合适的 CAT 和
  下一个可用序号。中文用例名跟在编号后：`M3-SEN-02_温湿度传感器`。
- **任务 prompt**：一句/一段自然语言需求（遵守第一铁律）。
- **验证类型**（决定验收点怎么写）：
  - **纯文本/网络类**：只靠串口日志 + ccglass 就能判定（如 M1-NET 系列）。
  - **外设验收类**：需要摄像头 VQA 拍照/录像、和/或 GPIO 注入（TESTER 板）来验证
    屏幕显示、按键动作（如时钟 App、计数器 App）。
  - 一个用例可以同时是两者（LLM 探索着把 App 写出来，验收靠 VQA/GPIO）。
- **通过标准 / iter 上限**：升 L1 的判据（默认：连测 3 次全过 + iter ≤ 4~6）。

### 2. 决定生成哪些文件

`test_spec.md` **永远必有**。其余按需：

| 文件 | 何时需要 |
|---|---|
| `test_spec.md` | 必有。完整测试规格（见 §3）。任务 prompt 就写在这里的"下发任务"步骤。 |
| `SKILL.md` | 仅当测试需要设备**预装一个 runtime skill** 才有（如一份 API 参考文档，供 claw 自主发现）。参考 `M1-NET-01/SKILL.md`。简单外设/App 类（GPIO 按键、时钟闹钟）**不需要**。 |
| `scripts/` | 仅当用例附带**参考 Lua 脚本**（作为 runtime skill 的一部分安装，或作为"直接跑某脚本验证驱动"的目标）。这些脚本绝不粘进任务 prompt。 |
| `BOARD.md` | 双板/接线复杂的外设用例，写引脚连接、DUT/TESTER 角色。参考 `M1-APP-03/BOARD.md`。 |

> 判断口诀：**外设显示/按键类**通常是 `test_spec.md`（+ 可选 `BOARD.md`），**不**带
> SKILL.md；**需要预装 API 文档/脚本供 claw 发现**的类才带 `SKILL.md`(+`scripts/`)。

### 3. 撰写 test_spec.md

严格按 `references/test_spec_template.md` 的骨架写。**必有的段落**（缺一不可）：

1. **顶部引用块** —— 链接 `harness_test_skill.md`（8 步闭环）；若有 SKILL.md 链接它 +
   `how_to_install_skill.md`；写明升 L1 判据。
2. **前置条件** —— 固件/网络/capability/外设/初始状态。
3. **测试步骤（每轮）** —— 顺序固定为**验收 → 回修 → 自我汇报 → CC 研判**，必须含：
   ① 清空会话 `AT+CLAW=session,clear,all`；
   ② 下发任务（放**自然语言 prompt**）；
   ③ 记录结果 + **第一轮验收**（跑验收点清单，逐条 Pass/Fail）；
   ④ **问题回修循环**（仅验收有 Fail 时：**只向 claw 描述现象、绝不说修法**，让它自己诊断修复→重验，最多 3 轮，仍不过则记为 finding）；
   ⑤ **自我汇报**（见 §5，措辞照抄，**每个 case、每一轮都执行，无论 Pass/Fail**，放在回修之后）；
   ⑥ **CC 研判 + 归档**（见 §5，Claude Code 独立判别真伪并补漏，产出合理 harness 改进点并归档到 `test_report/`）。
4. **期望的 LLM 自主行为** —— 用箭头流程写出你**猜测**它会怎么探索（不提示它，只用于
   对照观察它实际是否这么做、在哪一步卡住）。
5. **验收点清单** —— 见 §4，每个验收点绑定具体工具，客观可判定。
6. **整体 Pass 判定** —— 哪些必须过、iter 上限、3/3 升 L1。
7. **结束后的后续步骤** —— deactivate / 卸载 skill / 停 Lua job 等收尾。
8. **常见问题预判** —— 现象 / 可能原因 / 建议排查 三列表，帮跑测的人快速定位。

### 4. 写好验收点

验收点是"claw 到底跑通没有、质量如何"的客观判据。每一条要：
- **绑定明确的验证工具**：串口日志 / `mcp__ccglass__request_detail` / VQA 拍照 /
  VQA 录像 / GPIO 注入。
- **通过标准可判定**（出现某字符串、数字关系、VQA 逐字读到某内容），不要主观形容词。
- 按维度分组（A. 回复内容 / B. 调用轮次 / C. 数据一致性 …），每组一张小表。
- 静态显示用 VQA 拍照，"随时间刷新/动画/残影"用 VQA **录像**（`--video`）。

**VQA 命令一律用占位符** `<VQA_SCRIPT>`（因为每台机器的 vqa.sh 路径不同），并在
test_spec 顶部前置条件里提醒用户填入真实路径。例如：

```bash
<VQA_SCRIPT> "逐行照抄屏幕所有文字，乱码标[乱码]，不推断" home_layout
<VQA_SCRIPT> --video 6 "观察秒数是否每秒递增；是否只有时钟行刷新；有无残影" home_tick
```

GPIO 注入命令照写实际 pin：`AT+CLAW=gpio_ctrl,PA_25,click`（发给 TESTER 板）。
VQA / GPIO 的完整用法不要重复写，链接到 `harness_test_skill.md` 的速查区即可。

### 5. 自我汇报 + CC 研判（本项目核心目的）

**回修铁律：只说现象，不说修法。** 验收发现 Fail = claw 第一次没做好。回修时只向它描述
"哪条验收没过 / 什么现象"，**绝不提示根因、Lua 写法、API、pin 配置**——否则等于替它写代码，
还会污染它后续的自我反思。它凭现象自己做第二次修改，再验。

**自我汇报**是 harness 障碍收集的主入口。**每个用例、每一轮都要执行**（无论 Pass/Fail），
放在验收+回修**之后**，在同一 session 内、不清历史发送（让 LLM 回顾本轮完整上下文——包括
它第一次为什么失败、第二次怎么改的，这才是有价值的反思素材）。措辞照抄，别改写：

```
AT+CLAW=ask,请回顾你刚才完成这个任务的过程，列出你遇到的所有问题和障碍：
工具调用失败、API不符合预期、文档缺失、文件系统限制、让你多花轮次的任何障碍。
请诚实详细汇报，这对改进系统很重要。
```

**CC 研判**紧随其后（Step 6），是 test_spec 必写的一步：claw 的自我汇报**未必抓得住实质
问题**——可能漏报真障碍，也可能把非问题当问题。所以 Claude Code 必须**独立研判、不照单
全收**：

- 核实 claw 报的每一条（结合串口 narration / ccglass / 验收结果，判断真伪与根因层次：
  工具描述 / cap 行为 / driver / 文档缺失 / 还是 Lua 层自身）；**只把复述我们 Step 4 现象、
  未真正诊断根因的条目降权**；
- 从 iter 轮次、重复试错、失败日志里补 claw 没汇报出来的障碍；
- 产出**核实成立且合理**的 harness 改进点，区分"描述/文档类可直接改"与"需评估的
  cap/driver/架构改动"，并**归档到 `test_report/`**（规范命名，如
  `harness_report_<里程碑-分类>_<YYYYMMDD>.md`）。

> 研判/修复的**详细方法论、报告模板、编译回测机制**属于"执行 test case"的流程（见
> `harness_test_skill.md` Step 6–8，及未来的执行 skill）。本生成 skill 只需让 test_spec
> **点到并引用**它们，不要把整套方法论抄进每个 test_spec。

### 6. 注册到 test_case_list.md

打开 `ameba_claw/internal_project_ctrl/test_case_list/test_case_list.md`：
- 在对应里程碑/分类的表格里加一行（列对齐现有格式：编号 / 用例名 / 验证目标或功能描述 /
  依赖或操作 / 通过标准 / 通过 / 通过日期）。新用例"通过"列填 `—`。
- 更新底部"统计汇总"表的用例数与说明。
- 编号、名称、文件夹名三者一致。

## 参考文件

- `references/test_spec_template.md` —— test_spec.md 的完整带注释骨架（含纯网络类
  与外设验收类两种验收段落示例）。**写 test_spec 前先读它。**
- `references/conventions.md` —— 编号规则、目录/文件命名、L1/L2/L3 等级定义、
  VQA 占位符约定、注册清单的列格式、常见坑。

**活样例**（生成前建议对照）：
- 纯网络类范本：`ameba_claw/internal_project_ctrl/test_case_list/M1/M1-NET-01_实时IP信息/`
  （`test_spec.md` + `SKILL.md`）。
- 外设验收类范本：`.../M1/M1-APP-01_时钟闹钟App/test_spec.md`（VQA + GPIO 验收，无 SKILL.md）
  与 `.../M1-APP-03_点屏计数器/`（含 `BOARD.md`）。

## 完成后

告诉用户：新建了哪个文件夹、含哪些文件、在 `test_case_list.md` 的哪个里程碑下注册了。
提醒：外设类用例记得把 `<VQA_SCRIPT>` 替换成本机 vqa.sh 路径。**不要**自作主张去
执行这个测试——执行是另一条独立流程（见 `harness_test_skill.md`）。
