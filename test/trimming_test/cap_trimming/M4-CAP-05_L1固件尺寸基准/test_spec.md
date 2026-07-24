# M4-CAP-05 固件尺寸基准

记录各受控 cap 单独关断时的 Flash / BSS 节省量，建立尺寸基准数据。

## 方法

对每个受控 cap，各执行一次"全量 + 关一个"的编译，比较 `app.bin` 大小和 `.bss` 段大小。

**全量基准**（确保所有 cap 为默认值）：
```bash
./ameba.py menuconfig -r   # 重置到 default.conf + prj.conf
./ameba.py build -a .
ls -l build_RTL8721F/app.bin
size build_RTL8721F/build/project_km4tz/asdk/image/target_img2.axf | tail -1
```

**各 cap 关断**（重复以下流程 8 次，每次只改一个 symbol）：

```bash
# 示例：关闭 scheduler
./ameba.py menuconfig --set CLAW_CAP_SCHEDULER=n
./ameba.py build -a .
ls -l build_RTL8721F/app.bin
size build_RTL8721F/build/project_km4tz/asdk/image/target_img2.axf | tail -1

# 测完恢复
./ameba.py menuconfig --set CLAW_CAP_SCHEDULER=y
```

受控 cap 列表：`CLAW_CAP_SCHEDULER`、`CLAW_CAP_WEB_SEARCH`、`CLAW_CAP_HTTP_REQUEST`、`CLAW_CAP_VISION`、`CLAW_CAP_NET_DISCOVER`、`CLAW_CAP_AUDIO_STREAM`、`CLAW_CAP_MCP_CLIENT`、`CLAW_CAP_MCP_SERVER`

## 结果表

| 关断 cap | app.bin 节省 (KB) | BSS 节省 (KB) |
|----------|-------------------|----------------|
| 全量基准 | — | — |
| SCHEDULER | | |
| WEB_SEARCH | | |
| HTTP_REQUEST | | |
| VISION | | |
| NET_DISCOVER | | |
| AUDIO_STREAM | | |
| MCP_CLIENT | | |
| MCP_SERVER | | |

## Pass 标准

- [ ] 8 个 cap 数据全部填写完整
- [ ] 全量关断（M4-CAP-02 配置）Flash 节省 ≥ 80 KB 或 BSS 节省 ≥ 40 KB
