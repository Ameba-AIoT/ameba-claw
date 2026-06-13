# Board 管理模块用户手册

## 概述

Board 管理模块（`cap_board_mgr`）让 AI 助手了解你的硬件板子上连接了哪些设备、使用了哪些引脚，从而帮你生成正确的驱动代码和 Lua 脚本。

模块在系统启动时自动加载，默认使用内置的开发板配置；你也可以将自定义的 `board.json` 写入 VFS，运行时热加载生效，无需重新烧录。

---

## 快速开始

系统启动后无需任何配置，AI 已自动获知：

- 当前板子型号（如 `EV721FL0_R03`）
- 板子上已连接的设备（如 OLED 显示屏）
- 使用的接口和引脚（如 I2C0 → SDA: PA_26 / SCL: PA_25）

直接向 AI 提问即可：

> "帮我写一个在 OLED 上显示 Hello 的 Lua 脚本"
>
> "板子上有哪些可用的 SPI 实例？"
>
> "PA_8 这个引脚可以用吗？"

---

## AI 可调用的能力

| 能力 | 说明 |
|------|------|
| `board_list_devices` | 列出板子上所有设备的摘要（id、名称、类型、接口） |
| `board_get_device(id)` | 获取某个设备的完整信息：引脚分配、驱动参数、使用说明 |
| `board_query_peripheral(type)` | 查询某类外设的支持情况：实例数量、占用状态、可用引脚 |
| `board_schema` | 获取 board.json 编写规范，用于生成自定义配置 |
| `board_reload` | 从 VFS 重新加载 board.json，无需重启 |

AI 会根据你的问题自动选择调用哪个能力，通常不需要手动指定。

---

## 查询示例

### 查询所有设备

> "板子上有什么设备？"

AI 会调用 `board_list_devices`，返回类似：

```
板子 EV721FL0_R03 上共有 1 个设备：
- oled：SH1106 OLED 1.3"（display，通过 i2c0 连接）
```

### 查询设备详情

> "OLED 的 I2C 地址是多少？用的哪个引脚？"

AI 会调用 `board_get_device("oled")`，返回：

- 接口：I2C0，SDA = PA_26，SCL = PA_25，频率 400kHz
- 驱动参数：地址 0x3C，分辨率 128×64，col_offset = 2
- Lua 模块：i2c

### 查询外设可用性

> "我想再接一个 I2C 设备，有空闲实例吗？"

AI 会调用 `board_query_peripheral("i2c")`，返回：

- I2C0：已占用（被 i2c0 接口使用，连接 oled）
- I2C1：空闲，可用引脚列表：PA_6、PA_7 ...

---

## 自定义板子配置

### 步骤一：描述你的硬件

告诉 AI 你的板子与默认配置的差异：

> "我在 PA_8/PA_9/PA_10 接了一个 SPI 的 ST7789 LCD，帮我生成 board.json"

### 步骤二：AI 生成配置

AI 会调用 `board_schema` 获取格式规范，并生成如下内容：

```json
{
  "$chip": "RTL8721F",
  "$extends": "EV721FL0_R03",
  "interfaces": [
    {
      "id": "spi0",
      "type": "spi",
      "instance": "SPI0",
      "mosi": "PA_8",
      "miso": "PA_9",
      "sck": "PA_10"
    }
  ],
  "devices": [
    {
      "id": "lcd",
      "name": "ST7789 1.3\"",
      "type": "display",
      "chip": "ST7789",
      "interface": "spi0",
      "params": {
        "width": 240,
        "height": 240
      },
      "lua_module": "spi"
    }
  ]
}
```

### 步骤三：写入并热加载

> "把这个配置写入 VFS 并重新加载"

AI 会将文件保存到 `vfs:/board.json` 并调用 `board_reload`，立即生效。

### 恢复默认配置

删除 VFS 文件后重启，系统自动恢复内置默认配置：

> "删除 vfs:/board.json，然后重启系统"

---

## board.json 格式说明

### 顶层字段

| 字段 | 必填 | 说明 |
|------|:----:|------|
| `$chip` | 是 | 芯片型号，当前支持 `RTL8721F` |
| `$extends` | 否 | 继承的内置基础板名称（如 `EV721FL0_R03`）；`null` 表示从零定义 |
| `board` | 是 | 板子基本信息，见下表 |
| `board_pins` | 否 | 引脚列表 |
| `interfaces` | 否 | 接口定义数组 |
| `devices` | 否 | 设备定义数组 |

### board 对象

| 字段 | 必填 | 说明 |
|------|:----:|------|
| `name` | 是 | 板子名称 |
| `chip` | 是 | 具体芯片型号（如 `RTL8721FLM`） |
| `description` | 否 | 描述信息 |

### board_pins 对象

| 字段 | 说明 |
|------|------|
| `available` | 板子物理引出的引脚列表（字符串数组） |
| `remove_pins` | 从继承的父板中移除的引脚（仅在 `$extends` 时有意义） |

### interfaces 数组

每个接口对象的通用字段：

| 字段 | 必填 | 说明 |
|------|:----:|------|
| `id` | 是 | 接口唯一标识（如 `i2c0`、`spi0`） |
| `type` | 是 | 接口类型（见下表） |
| `instance` | 是 | 芯片外设实例（如 `I2C0`、`SPI1`） |
| `$op` | 否 | 填 `"remove"` 可从父板删除该接口 |

各类型专用引脚字段：

| type | 引脚字段 |
|------|---------|
| `i2c` | `sda`、`scl`、`default_freq_khz` |
| `spi` | `mosi`、`miso`、`sck` |
| `uart` | `tx`、`rx`（可选：`cts`、`rts`） |
| `pwm` | 在 device `params` 中指定 `timer_idx`、`channel`、`pin` |
| `gpio` | 在 device `params` 中指定 `pin` |

### devices 数组

| 字段 | 必填 | 说明 |
|------|:----:|------|
| `id` | 是 | 设备唯一标识（如 `oled`、`lcd`） |
| `name` | 否 | 设备显示名称 |
| `type` | 否 | 设备类型（如 `display`、`sensor`） |
| `chip` | 否 | 设备芯片型号（如 `SH1106`、`ST7789`） |
| `interface` | 否 | 使用的接口 id |
| `params` | 否 | 驱动参数（地址、分辨率等，自由定义） |
| `lua_module` | 否 | 对应的 Lua 驱动模块名 |
| `notes` | 否 | 驱动注意事项（AI 生成代码时参考） |
| `usage_guide` | 否 | 详细使用指南 |
| `$op` | 否 | 填 `"remove"` 可从父板删除该设备 |

---

## 继承与覆盖规则

使用 `$extends` 时，子板配置与父板按以下规则合并：

| 内容 | 合并规则 |
|------|---------|
| `interfaces` / `devices` 数组 | 按 `id` 匹配：存在则替换，不存在则追加，`$op:"remove"` 则删除 |
| `board_pins.available` | 追加去重（不删除父板引脚） |
| `board_pins.remove_pins` | 从父板 `available` 中删除指定引脚 |
| `board` 对象字段 | 逐字段覆盖 |
| 其他标量/数组 | 子板完全覆盖父板 |

**示例：从父板移除引脚并新增设备**

```json
{
  "$chip": "RTL8721F",
  "$extends": "EV721FL0_R03",
  "board_pins": {
    "remove_pins": ["PA_25", "PA_26"],
    "available": ["PA_8", "PA_9", "PA_10"]
  },
  "interfaces": [
    { "$op": "remove", "id": "i2c0" },
    {
      "id": "spi0",
      "type": "spi",
      "instance": "SPI0",
      "mosi": "PA_8",
      "miso": "PA_9",
      "sck": "PA_10"
    }
  ],
  "devices": [
    { "$op": "remove", "id": "oled" }
  ]
}
```

---

## 引脚可用性说明

`board_query_peripheral` 返回的 `available_pins` 列出**所有物理引出的引脚**（板级 `board_pins.available` 减去芯片系统保留引脚），包含已被接口占用的引脚。AI 需要结合 `instances` 字段中各实例的 `config`（引脚分配详情）来判断哪些引脚当前已在使用。

| 字段 | 含义 |
|------|------|
| `available_pins` | 板子所有物理引出引脚（含已占用），仅排除片内 Flash/PSRAM 系统保留引脚 |
| `instances.<ID>.status` | `"occupied"` 或 `"free"`，表示该外设实例当前是否已被接口使用 |
| `instances.<ID>.config` | 已占用实例的完整配置（引脚分配、频率等参数） |

直接询问即可获得推荐：

> "给我推荐一个空闲的 UART 实例和可用的引脚组合"
>
> "哪些引脚可以用来接 SPI 设备？"

---

## 编译期 board 选择

固件编译时，所有 `boards/` 子目录下的 board.json 都会被嵌入到固件中，但只有一个会被选为"默认 board"——当 VFS 中不存在 `board.json` 时，系统会自动将默认 board 写入 VFS。

### 当前内置板

| 内置板名称 | 说明 |
|-----------|------|
| `EV721FL0_R03` | RTL8721F 开发板基础定义（I2C0 接口，无设备） |
| `EV721FL0_R03_BreadBoard` | 面包板扩展版：OLED + 按键 A/B/C（继承自 EV721FL0_R03）|

### 指定默认 board

通过 `-D BOARD=<名称>` 在编译时指定：

```bash
# 使用面包板扩展版作为默认 board
python ameba.py build -a /path/to/ameba_claw -D BOARD=EV721FL0_R03_BreadBoard

# 使用基础板（默认行为，也可显式指定）
python ameba.py build -a /path/to/ameba_claw -D BOARD=EV721FL0_R03
```

不指定 `BOARD` 时，按字母顺序取第一个（即 `EV721FL0_R03`）。

传入不存在的 board 名称时，cmake 配置阶段会报错并列出所有可用 board。

### 添加自定义内置板

在 `boards/<板子名>/board.json` 创建新目录和文件，重新编译固件后该板子自动加入候选列表，可通过 `-D BOARD=<板子名>` 选择。

---

## 当前支持的芯片与内置板

| 类型 | 名称 |
|------|------|
| 芯片 | `RTL8721F` |
| 内置板 | `EV721FL0_R03`、`EV721FL0_R03_BreadBoard` |

---

## 常见问题

**Q：修改了 board.json 后没有生效？**  
A：调用 `board_reload` 或让 AI 执行"重新加载板子配置"。如果 VFS 文件格式有误，会回退到已加载的配置，日志中会有错误提示。

**Q：想从零定义板子，不继承任何内置板？**  
A：将 `$extends` 设为 `null`，同时完整填写 `board`、`board_pins`、`interfaces`、`devices` 字段。

**Q：设备的 `params` 字段有哪些固定格式？**  
A：`params` 是自由结构的 JSON 对象，内容完全由你和对应驱动约定，AI 生成代码时会将其原样传递给驱动。

**Q：重启后自定义配置会丢失吗？**  
A：不会。配置保存在 VFS（Flash 文件系统）中，重启后优先加载 `vfs:/board.json`，只有该文件不存在时才使用内置默认配置。
