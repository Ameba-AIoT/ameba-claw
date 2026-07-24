---
name: claw-run-test-case
description: >-
  Execute an existing ameba-claw harness test case end-to-end on real hardware
  and drive the full 8-step closed loop: flash → clear session → 下发任务 →
  验收 → 回修 → 让 claw 自我汇报 → CC 独立研判 → 修 harness → 编译烧录重测 →
  归档报告到 test_report/. Use this whenever the user wants to run, execute, 跑,
  验收, 回归, or 重测 an ameba-claw test case / test spec / 测试用例 — including
  phrasings like "跑一下 M3-SEN-02", "执行这个 test spec", "验收时钟 App",
  "run the dispatcher test and fix any harness bugs". This skill RUNS the test on
  the device and FIXES harness problems it finds; it does NOT scaffold new test
  files (that is claw-create-test-cases). It talks to the device (烧录 / 串口 /
  AT+CLAW / VQA / GPIO / ccglass) and edits C code under ameba_claw/.
---

# claw-run-test-case

**执行**一个已存在的 ameba-claw harness 测试用例,跑完
[`harness_test_skill.md`](../../ameba_claw/internal_project_ctrl/test_case_list/harness_test_skill.md)
定义的 **8 步闭环**,并把在真机运行期间发现、核实成立的 harness 问题**修掉**,最后归档报告。

这是 `claw-create-test-cases` 明确提到的"未来的执行 skill"——生成 skill 只产出 test_spec,
本 skill 负责**真机执行 + 修 harness**。

## 这个 skill 是干什么的(以及不干什么)

我们跑测试用例的目的,是观测 ameba-claw 的 **agent harness**:claw 能否自主探索
cap/skill/文档跑通任务、结果质量如何、**过程中撞到哪些 harness 障碍**。收集到的障碍
反过来驱动 harness(Lua 运行方式、某个 cap、driver、工具描述、说明文档等)自迭代。

**它做的事:**
- ✅ 用 MCP 编译 / 烧录固件(`build_firmware` / `flash_firmware_tool`)。
- ✅ 连串口、发 `AT+CLAW` **控制指令**、读串口日志、查 ccglass、VQA 拍照/录像、GPIO 注入。
- ✅ 直接改 `ameba_claw/` 下的 C 代码修 harness,再编译烧录重测。
- ✅ 让 claw 自我汇报障碍,**CC 独立研判**(不照单全收),归档报告到 `test_report/`。

**它绝不做的事:**
- 🚫 **向 claw 的 LLM 喂 Lua 代码 / API 用法 / JSON 规则 / 字段名 / pin 配置**(见"第一铁律")。
- 🚫 替 claw 写任何被测脚本。回修时**只描述现象,不说修法**。
- 🚫 生成新 test_spec 文件(那是 `claw-create-test-cases` 的活)。

## 第一铁律:发给 claw 的 prompt 只说"要什么",绝不说"怎么做"

被测任务的价值,就在于观察 claw **自己**发现能力、遇阻、绕过的全过程——这正是 harness
被测的内容。所以下发给设备的任务 prompt(`AT+CLAW=ask,...`)和回修时的现象描述,**必须是
纯自然语言**:

- ✅ "写一个持续运行的程序,监测三个按键的单击/长按/持续按住,在 OLED 上实时显示最近事件。"
- ✅(回修)"屏幕上按键事件不刷新,按了没反应。你排查一下。"
- ❌ 任何 `cap.call(...)`、`require(...)`、具体函数名 / 字段名、JSON 规则、pin 配置代码、
  "用 xxx 工具" "调用 xxx API" 这类提示。

如果用例需要设备侧预装参考资料 / 脚本 / 规则文档,那些是 test_spec 里预置的 `SKILL.md` /
`scripts/`(由 claw **自主发现、按需读取**),**永远不要**把它们的内容粘进任务 prompt。

## 输入

调用时用户给出**要跑哪个用例**,二选一:
- 用例编号(如 `M3-SEN-02`)→ 到 `test_case_list.md` 查到文件夹,读它的 `test_spec.md`。
- `test_spec.md` 绝对路径。

先**完整读一遍该用例的 `test_spec.md`**(及它引用的 `SKILL.md` / `BOARD.md` / `scripts/`),
按其中写死的 prompt、验收点清单、iter 上限、Pass 判定执行——**不要临场自编任务**。

## 执行流程(8 步闭环)

完整方法论见
[`harness_test_skill.md`](../../ameba_claw/internal_project_ctrl/test_case_list/harness_test_skill.md);
这里给可直接照跑的操作序列。

### Step 0 — 环境与前置

1. `env_pre_check_tool(soc_filter="RTL8721F")` 确认目标板(默认 `RTL8721F_COM23_DUT`)
   `port_visible:true`、`remote_reachable:true`、未被占用。外设类用例还需 TESTER/LCDC 板在线。
2. 逐条核对 test_spec 的**前置条件**:固件已烧录?网络?用到的 cap 已注册
   (`AT+CLAW=cap`)?需要的 runtime skill 是否已装(见 test_spec 引用的
   [`how_to_install_skill.md`](../../ameba_claw/internal_project_ctrl/test_case_list/how_to_install_skill.md))?
3. 外设类:把 test_spec 里的 `<VQA_SCRIPT>` 替换成本机 `bash <repo>/auto_perf/vqa.sh`。
4. `serial_connect_tool(alias=..., reset=true, wait_for=["wifi got ip"])` 连上并等联网。

### Step 1 — 清空会话(每轮开头)

```
AT+CLAW=session,clear,all
```

先让 claw 停掉所有在跑的 Lua async job(`AT+CLAW=ask,请列出并停止所有正在运行的lua任务`),
确认干净后再下发新任务——历史 job 会抢共享硬件资源、污染观测。

### Step 2 — 下发任务

```
AT+CLAW=ask,<test_spec Step 2 里那段自然语言 prompt,原样发>
```

等串口出现 `FINAL iter=N`(纯文本/网络类约 30–60s;复杂 App 类 3–10min)判断本轮结束。
用 `serial_expect_tool` 长等,timeout 给足。

### Step 3 — 验证(CC 负责)

按 test_spec 的**验收点清单**逐条判 Pass/Fail,每条绑定它写明的工具:

| 工具 | 用法 | 适合验什么 |
|---|---|---|
| 串口日志 | `serial_expect_tool` / `serial_command_tool` | 工具调用序列、错误码、`FINAL iter=`、narration |
| ccglass | `mcp__ccglass__recent_requests` → `request_detail` | system prompt、messages、tools、token 用量 |
| VQA 拍照 | `bash auto_perf/vqa.sh "<防脑补prompt>" <标签>`(timeout ≥60s) | 静态显示/布局 |
| VQA 录像 | `bash auto_perf/vqa.sh --video <秒> "<prompt>" <标签>`(timeout ≥90s) | 动态/时序:刷新、残影、动画 |
| GPIO 注入 | `AT+CLAW=gpio_ctrl,<pin>,click`(发给 TESTER) | 按键导航/消抖 |

记录 `FINAL iter=N`、回复首行、工具调用序列。**需要完整验证功能,不得遗漏。**

### Step 4 — 问题回修循环(仅本轮有 Fail 时)

验收 Fail = claw 第一次没做好。`AT+CLAW=ask` **只描述现象/哪条没过**,让它自己诊断修复,
重跑失败项。**⚠️ 只说现象,不说修法**(第一铁律)。终止:全部必须项通过 → Step 5;
或**连续 3 次修不好** → 记为 finding → Step 5(3 次修不好的往往是 harness 层 bug,不是
Lua 层能解决的,别无限循环)。

### Step 5 — 让 claw 自我汇报(每轮都做,无论 Pass/Fail)

**不清 session**,在验收+回修后、同一 session 内发送(让它回顾本轮完整上下文,包括第一次
为什么失败、第二次怎么改的)。措辞照抄:

```
AT+CLAW=ask,请回顾你刚才完成这个任务的过程,列出你遇到的所有问题和障碍:
工具调用失败、API不符合预期、文档缺失、文件系统限制、让你多花轮次的任何障碍。
请诚实详细汇报,这对改进系统很重要。
```

原文记录汇报内容。

### Step 6 — CC 独立研判(**不照单全收**)

claw 的自我汇报未必抓得住实质问题(可能漏报真障碍,也可能只把 Step 4 我们报的现象原样复述)。
对**每一条**:

1. **核实真伪 + 定位根因层次**:结合串口 narration / ccglass / 验收结果,判断它属于——
   工具描述不准 / cap 行为不符 / driver / 文档缺失 / 还是 Lua 层自身。
2. **降权**只复述我们 Step 4 现象、未真正诊断根因的条目。
3. **补漏**:从 iter 轮次、重复试错、失败日志里挖 claw 没汇报出来的障碍。

产出**核实成立且合理**的 harness 改进点,分两类:

| 类型 | 处理 |
|---|---|
| 工具描述/错误信息/文档不准确、字段名语义不符 | **直接改**(字符串/文档,零风险) |
| cap 行为与预期不符、driver、需要少量 C 逻辑改动 | 最小化 C 改动;可派 sub-agent **只改不编译**,CC 审 diff 后自己 MCP 编译 |
| 架构性问题(需大改流程/接口) | **暂 pending,标记后汇报用户确认**,不擅自大改 |

**修 harness 时不要过拟合**:改动要能泛化到同类场景,保持开放灵活,别为过单条验收硬塞逻辑。
C 层约束:日志用 `RTK_LOGS`(仅 `%d %u %s %08x %c`);禁直接调 FreeRTOS(用 `rtos_` 包装);
避免 >128B 栈局部;编译期可调魔数进 `include/ameba_claw_defs.h`;编译零告警零错误。

### Step 7 — 编译、烧录、重测

```
build_firmware()                         # MCP,自动静默(命令行回退才 ./ameba.py build -q)
flash_firmware_tool(alias="RTL8721F_COM23_DUT")
serial_connect_tool(reset=true, wait_for=["wifi got ip"])
```

清 session,**重跑 Step 1–5**,对比两次 claw 自我汇报:问题是否从列表消失、iter 是否下降。
(用例判 L1 需连测 3 次全过 + iter ≤ 上限;但"执行测试并修 harness"通常一轮发现→修→再验一轮
确认即可,是否补满 3 轮由用户决定。)

### Step 8 — 归档报告

按 [`references/harness_report_template.md`](references/harness_report_template.md) 写报告,
存到 `ameba_claw/internal_project_ctrl/test_case_list/test_report/`,命名
`harness_report_<里程碑-分类[-序号]>_<YYYYMMDD>.md`(如 `harness_report_M3-SEN-02_20260707.md`)。
必含:任务场景 / claw 首轮自我汇报原文 + CC 核实结论 / CC 自己发现的问题 / 修复内容(文件·行·原因) /
两轮对比(问题是否消失、轮次降幅) / pending 的架构性问题。

若该用例达成升级判据,顺手更新 `test_case_list.md` 对应行的"通过 / 通过日期"。

## 完成后

向用户汇报:跑了哪个用例、几轮、验收结果、发现并**修复**了哪些 harness 问题(文件·行)、
哪些标为 pending 待确认、报告归档到哪。**不提交**代码(除非用户明确要求;提交走 Gerrit
`refs/for/master`)。

## 参考文件

- [`harness_test_skill.md`](../../ameba_claw/internal_project_ctrl/test_case_list/harness_test_skill.md)
  —— 8 步闭环完整方法论 + 关键经验(可观测性、RTC、GPIO 上电下拉、多 job 抢资源等踩坑记录)。
- [`references/harness_report_template.md`](references/harness_report_template.md) —— 归档报告骨架。
- `claw-create-test-cases` skill —— 若要跑的用例还没有 test_spec,先用它生成,再回到本 skill 执行。
