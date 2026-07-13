# 测试计划：checkpoint 持久化 + WebUI 会话管理

**对应改动：** 当前 unstaged 修改（claw_memory、claw_agent、cap_im_local、cap_webui、ameba_claw_main）  
**日期：** 2026-07-08

---

## 一、背景与改动范围

本次改动涉及四个功能域：

| 域 | 核心改动 | 风险点 |
|---|---|---|
| A. checkpoint 持久化 | claw_memory 新增 `completed` 字段与 upsert 语义 | 写入逻辑分支多，边界条件复杂 |
| B. VFS fallback | cap_im_local push_session_history 重启后回退读 VFS | 路径计算与格式解析 |
| C. 跨 session 路由修复 | NO_ACK flag + on_tool_progress 按 source_message_id 路由 | 并发下可能仍有竞态 |
| D. WebUI 会话管理页 | session_mgr.js 增删查历史，esc() XSS 修复 | djb2 哈希与固件不一致 |

---

## 二、测试用例列表

### 域 A：checkpoint / upsert / completed 字段

#### A-01：首次写入 completed:true
- **前置：** 清空 session 文件
- **操作：** 以 assistant_text≠NULL、request_id=1001 调用 `claw_memory_append_session_turn`
- **断言：**
  - VFS 文件存在，turns 数组长度为 1
  - turns[0] 包含 `"completed":true`
  - turns[0] 包含 `"req_id":1001`
  - turns[0] 包含 `"user"` 和 `"assistant"` 字段

#### A-02：写入 checkpoint 转（assistant_text=NULL）
- **操作：** 以 assistant_text=NULL、request_id=1002 调用 append
- **断言：**
  - turns[0] 包含 `"completed":false`
  - turns[0] **不含** `"assistant"` 字段（区分于空字符串）
  - turns[0] 包含 `"req_id":1002`

#### A-03：upsert 命中——同 request_id 再次写入
- **前置：** A-02 已写入 turns[0]（completed:false, req_id=1002）
- **操作：** 以 assistant_text="final reply"、request_id=1002 再次调用 append
- **断言：**
  - turns 数组长度**仍为 1**（未追加新条目）
  - turns[0] 变为 `"completed":true`
  - turns[0] 包含 `"assistant":"final reply"`
  - turns[0] **仍含** `"user"` 字段（不被清空）

#### A-04：upsert 不命中——不同 request_id
- **前置：** turns[0] 为 completed:true，req_id=1001
- **操作：** 以 request_id=1002 调用 append
- **断言：** turns 数组长度为 2（追加，不替换）

#### A-05：upsert 不命中——request_id=0（禁用 upsert）
- **操作：** 以 request_id=0 重复调用两次 append
- **断言：** turns 数组长度为 2（每次均追加）

#### A-06：多轮 tool-loop 不膨胀
- **操作：** 以相同 request_id=3001 连续写入 3 次（模拟 3 轮工具调用）
  - 第 1、2 次：assistant_text=NULL（checkpoint）
  - 第 3 次：assistant_text="done"（终结）
- **断言：** turns 数组长度为 1，且 completed:true

#### A-07：upsert 时 tool_msgs 字段被刷新
- **前置：** turns[0] 为 checkpoint，tool_msgs=[round1]
- **操作：** upsert 写入 tool_msgs=[round1, round2]
- **断言：** turns[0].tool_msgs 包含两轮，而非仅第一轮

#### A-08：ring buffer 限制仅对 append 生效，upsert 不触发驱逐
- **前置：** session 已满（例如 10 条 turns，max_session_turns=10）
- **操作：** 以已有 req_id 执行 upsert
- **断言：** turns 数量不变（10），最老条目未被驱逐

#### A-09：ring buffer 对新 append 仍正常驱逐
- **前置：** session 已满（10 条 turns）
- **操作：** 以新 request_id=0 调用 append
- **断言：** turns 数量仍为 10（最旧条目被驱逐）

#### A-10（corner case）：空 turns 数组时 upsert 退化为 append
- **前置：** turns 数组为空，request_id=1001
- **操作：** 调用 append（预期 upsert 检查找不到 last turn）
- **断言：** turns 数组长度为 1（首次写入）

#### A-11（corner case）：session 文件不存在时正常创建
- **操作：** 删除 session 文件，然后调用 append
- **断言：** 文件被创建，turns 数组含 1 条记录

---

### 域 B：collect_session_history 与 VFS fallback

#### B-01：collect_session_history 跳过 completed:false
- **前置：** VFS session 文件含 1 条 completed:false + 1 条 completed:true
- **操作：** 触发对该 session 的 LLM 请求（通过 `AT+CLAW ask`）
- **断言（serial log）：** context 中仅包含 completed:true 的 user/assistant 内容，incomplete 条目不出现

#### B-02：collect_session_history 对无 completed 字段的旧格式兼容
- **前置：** VFS 文件只有无 completed 字段的旧格式 turns
- **操作：** 触发 LLM 请求
- **断言：** context 中包含这些 turns（向后兼容，视为 complete）

#### B-03：VFS fallback——新 alias 同步返回持久化历史
- **前置：** 创建 alias（REST POST new），**不发任何消息**；直接向 VFS 写入含 1 条 completed:true turn 的 session 文件
- **操作：** WebSocket sync {type:"sync", alias:X}
- **断言：** snapshot.messages 包含该条 turn 的 user/assistant text

#### B-04：VFS fallback——completed:false 展示为 "interrupted" 提示
- **前置：** VFS 文件含 1 条 completed:false turn（user="q1"）
- **操作：** WS sync（alias 无 RAM 数据）
- **断言：**
  - snapshot.messages 含 role:user text:"q1"
  - snapshot.messages 含 role:assistant text:"ameba claw is interrupted!"
  - 无空字符串 text 条目

#### B-05：VFS fallback——旧格式 turn 正常展示（无 interrupted 提示）
- **前置：** VFS 文件含 1 条无 completed 字段的 turn
- **操作：** WS sync
- **断言：** 展示正常 user/assistant，无 "ameba claw is interrupted!"

#### B-06：VFS fallback——混合 turns（complete + incomplete + legacy）
- **前置：** VFS 文件含 3 条 turns：complete、legacy、incomplete
- **操作：** WS sync
- **断言：**
  - 共 6 条 messages（3×2）
  - 只有 1 条 "ameba claw is interrupted!"（对应 incomplete）
  - complete 和 legacy 均正常展示

#### B-07（corner case）：VFS 文件不存在——返回空 snapshot，不崩溃
- **操作：** WS sync 一个无 RAM 数据且无 VFS 文件的新 alias
- **断言：** 返回 snapshot，messages 为空数组

#### B-08（corner case）：VFS 文件内容为空 turns 数组
- **前置：** 写入 `{"turns":[]}`
- **操作：** WS sync
- **断言：** messages 为空数组

#### B-09（corner case）：VFS 文件 JSON 格式损坏
- **前置：** 写入 `{invalid]}`
- **操作：** WS sync
- **断言：** 固件不崩溃，返回空 snapshot（不挂起）

#### B-10（corner case）：user 字段为空字符串的 turn
- **前置：** turns[0] = `{"user":"","assistant":"reply","completed":true}`
- **操作：** WS sync
- **断言：** snapshot 仍返回（不跳过），user text 为空字符串

---

### 域 C：跨 session 路由修复 + NO_ACK

#### C-01：NO_ACK——发送消息后无早期 ACK 帧
- **前置：** server current = alias A；WS 连接监听
- **操作：** 发送消息 `{text:"...", alias:B}`，等待 4 秒
- **断言：** 4 秒内 alias A 和 B 的 WS 连接均未收到 role:assistant 帧（LLM 响应通常 >20s）
- **注：** 原行为是发送 "working on it..." ACK 到 current（A），新行为禁止 ACK

#### C-02：on_tool_progress 路由到 source_message_id（tool 调用进度）
- **前置：** 多 session；当前 session 为 A；从 alias B 发送需多步工具的任务
- **操作：** 观察 WS 广播
- **断言：**
  - tool progress 帧带 alias=B
  - alias A 的 WS 连接不收到 B 的 tool progress 帧
- **测试类型：** 需 LLM（标记为 Manual）

#### C-03：切换 session 后 on_tool_progress 仍路由到发起 session
- **场景：** session B 发起任务；任务运行中切换到 session A（POST resume A）；等待响应
- **断言：** 最终 LLM 回复仍发到 alias B（不因切换而丢失到 A）
- **测试类型：** 需 LLM（标记为 Manual）

#### C-04：broadcast_session_snapshot 不再内嵌 history
- **操作：** POST action=resume（或 new/rename/clear）
- **断言：** WS 收到的 session_snapshot 帧中**不含** "history" 字段（已从接口移除）

#### C-05（corner case）：source_message_id 为空时 on_tool_progress 退回通用路由
- **场景：** 非 local channel（如 Telegram）发起任务
- **断言：** on_tool_progress 走原有 claw_im_dispatch_send_progress 路径，行为不变

---

### 域 D：WebUI 会话管理页面

#### D-01：loadSessionMgr 列出所有 channel 的 session
- **操作：** GET /api/files?path=/session/chat_map，枚举 s_*.json
- **断言：**
  - 返回 JSON 数组，每项含 name、path 字段
  - 文件名匹配 `s_*.json` 模式

#### D-02：loadSessionMgr 读取每个 chat_map 文件的结构
- **操作：** GET /api/files/content?path={map_file}
- **断言：** JSON 包含 `sessions`（数组）和 `current`（字符串）字段

#### D-03：smViewHistory 读取 session 文件中的 turns
- **前置：** VFS session 文件含 2 条 turns
- **操作：** GET /api/files/content?path={session_file}
- **断言：**
  - JSON 含 `turns` 数组
  - 每条 turn 含 `user`、`assistant`（或无 assistant 对于 incomplete）

#### D-04：smViewHistory 处理 completed:false（不崩溃，展示空 assistant）
- **前置：** turns[0] = incomplete turn（无 assistant 字段）
- **操作：** 读取文件后渲染（`t.assistant||''`）
- **断言：** 读取不报错，assistant 字段缺失时 JS 退化为空字符串（`t.assistant||''`）
- **备注：** smViewHistory 直接显示原始字段，**不**做 "interrupted" 转换（这是服务端 VFS fallback 的职责）

#### D-05：smDelete 删除非当前 session——文件和 map 均更新
- **前置：** 两个 sessions（A current，B 非 current）
- **操作：**
  1. DELETE /api/files?path={session_file_B}
  2. GET /api/files/content?path={map_file}，移除 B，PUT 回
- **断言：**
  - session_file_B 已不存在（GET 返回 404）
  - map 文件 sessions 不含 B
  - map 文件 current 仍为 A

#### D-06：smDelete 删除当前 session——current 更新为首个剩余 alias
- **前置：** sessions=[A(current), B]
- **操作：** 同上，删除 A，将 current 改为 B
- **断言：** map 文件 current=B

#### D-07：smDelete 删除最后一个 session——current 置为 "default"
- **前置：** sessions=[A(current)]
- **操作：** 删除 A，sessions 变为空，current=sessions.length?sessions[0]:'default'
- **断言：** map 文件 current="default"，sessions=[]

#### D-08（corner case）：smDelete 删除不存在的 session 文件——map 仍正常更新
- **操作：** DELETE /api/files?path={不存在的路径}（预期 404）；继续 PUT map
- **断言：** map 文件依然被正确更新（DELETE 失败不阻断 map 更新）

#### D-09（corner case / 潜在 bug）：JS djb2 与固件 djb2 哈希函数不一致
- **背景：** session_mgr.js 的 `djb2()` 使用 XOR variant（`(h*33)^c`），而固件和 Python 测试套件使用 addition variant（`h*33+c`）。二者对同一字符串产生不同哈希，导致 `sessionFilePath()` 计算出错误路径，`smDelete` 会删除不存在的文件。
- **验证方法：** 对同一 session_id（如 `"local:local:default"`）分别用 JS 和 Python 计算哈希，与 VFS 实际文件路径对比
- **预期结果：** **应失败**（证实 bug）；修复方案：JS 改用 `(h*33+c) >>> 0`
- **优先级：** High——影响所有删除操作

#### D-10（corner case）：esc() 单引号转义
- **背景：** esc() 新增 `'` → `&#39;`，防止 onclick 中 JS 注入（如 alias 含单引号）
- **验证：** 创建 alias 含 `'` 或用户 text 含 `'` 的 session；在会话管理页渲染时检查 HTML 源码
- **断言：** onclick handler 中单引号被编码为 `&#39;`，不破坏 JS 语法

#### D-11：vfs: 路径前缀兼容性
- **背景：** session_mgr.js 使用 `vfs:/session/...` 路径（含 `vfs:` 前缀），而现有测试套件使用 `/session/...`
- **验证：** GET /api/files?path=vfs:/session/chat_map 与 GET /api/files?path=/session/chat_map 返回结果是否一致
- **断言：** 两者均成功返回相同文件列表（如一致，说明固件透明处理 `vfs:` 前缀；如不一致，session_mgr.js 路径错误）

---

### 域 E：event_dispatcher 队列满反馈

#### E-01：队列满时用户收到反馈消息
- **场景：** 模拟 agent 队列满（快速连续发送多条消息）
- **断言：** 发起方 channel 收到 "I'm too busy right now, please try again." 消息
- **测试类型：** 较难自动化（需构造队列满状态），建议 Manual

#### E-02：队列满日志包含 session_id
- **操作：** 触发队列满场景
- **断言：** serial log 中 "RUN_AGENT submit failed" 行包含对应 session_id

---

## 三、测试优先级汇总

| 优先级 | 用例 | 类型 |
|---|---|---|
| P0（阻断） | A-03 upsert 命中，A-06 多轮不膨胀，B-03 VFS fallback 基本，B-04 incomplete 展示，D-09 djb2 hash bug | 自动化 |
| P1（核心） | A-01/02/04/05，B-01 LLM 跳过 incomplete，C-01 NO_ACK，D-05/06/07 删除，D-11 vfs 前缀 | 自动化 + Manual |
| P2（健壮性） | A-07~11，B-06~10，C-02/03/04/05，D-01~04/08/10 | 自动化 |
| P3（Nice-to-have） | E-01/02 | Manual |

---

## 四、自动化边界说明

| 需要 LLM（Manual） | 不需要 LLM（可自动化） |
|---|---|
| B-01/02（collect_session_history 跳过逻辑） | A-01~11（直接操作 VFS 文件验证结构） |
| C-02/03（tool progress 路由，需工具调用） | B-03~10（WS sync + VFS 文件写入） |
| E-01/02（队列满构造） | C-01/04（NO_ACK、snapshot 无 history） |
| | D-01~11（REST + 文件 API） |

---

## 五、测试文件建议位置

```
test/
  rest_api_test/
    test_memory_checkpoint.py     # A系、B系（自动化部分）
    test_session_mgr_api.py       # D系（自动化部分）
    test_no_ack_routing.py        # C-01、C-04（自动化部分）
  manual/
    manual_checkpoint_upsert.md  # A-06的手动步骤（含 AT cmd 序列）
    manual_llm_context_skip.md   # B-01/02的手动步骤
    manual_tool_progress_route.md # C-02/03的手动步骤
```
