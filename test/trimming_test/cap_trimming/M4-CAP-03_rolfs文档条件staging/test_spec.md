# M4-CAP-03 rolfs 文档条件 staging

验证关闭 cap 后其 `docs/` 不进入 rolfs 镜像，LLM 无法读取对应文档。

## 配置（验证阶段：两个 cap 均关）

```bash
./ameba.py menuconfig --set CLAW_CAP_AUDIO_STREAM=n CLAW_CAP_NET_DISCOVER=n
```

## 验证步骤

**1. 编译**
```bash
./ameba.py build -a . 2>&1 | grep -E "error:|Build done|FAIL"
```

**2. 清理 staging/docs 并重建**（build 系统增量 copy，不删旧文件，必须先清理）
```bash
rm -rf build_RTL8721F/build/rolfs_staging/docs/
./ameba.py build -a . 2>&1 | grep -E "error:|Build done|FAIL"
find build_RTL8721F/build/rolfs_staging/docs -name "*.md" | sort
```
期望：`audio_stream.md` 和 `net_discover.md` 均不存在。

**3. 对照：恢复 net_discover，重编，确认文档回来**

```bash
./ameba.py menuconfig --set CLAW_CAP_NET_DISCOVER=y
./ameba.py build -a . 2>&1 | grep "Build done"
find build_RTL8721F/build/rolfs_staging/docs -name "net_discover*"
```
期望：`net_discover.md` 出现；`audio_stream.md` 仍缺席。

**4. 烧录对照固件，LLM 读取验证（可选）**

通过串口发送：
- `AT+CLAW=ask,读取rolfs:/docs/net_discover.md的内容`
- `AT+CLAW=ask,读取rolfs:/docs/audio_stream.md的内容`

期望：net_discover.md 返回内容，audio_stream.md 返回文件不存在。

## 恢复

```bash
./ameba.py menuconfig --set CLAW_CAP_AUDIO_STREAM=y CLAW_CAP_NET_DISCOVER=y
```

## Pass 标准

- [ ] 两 cap 关闭后 staging 目录无对应 .md
- [ ] net_discover 恢复后 staging 目录出现对应 .md
- [ ] LLM 读取结果与 staging 一致（可选）
