# Classic7 音频修复迁移交接包（R127/R128，2026-11-16）

## 1. 项目目标

让 Windows 桌面 iPod Classic 7 模拟器播放电脑可听见、节奏正常且无明显撕裂/爆音的音乐，同时保持 iPod 原生菜单、进度显示、休眠唤醒、歌曲与设置正常。

## 2. 当前状态

**未修好。** 正式文件未覆盖。R128宿主输出开关、采样率和一次有界调度对照已完成，均未解决稳态缺额。已正常关闭测试侧车并恢复正式程序，最终运行身份见同目录 `ROLLBACK.json`，切勿复用旧 PID。

### R128实测结果（2026-11-16）

| 模式 | 整段60秒 V/W | 末25秒 V/W | 末25秒PCM供样 |
|---|---:|---:|---:|
| 电脑输出关闭、保留1ms请求 | 98.17% | 94.21% | 输出环未启用，不能用wr计算供样 |
| 电脑44100Hz输出 | 97.21% | 94.52% | 166940 B/s |
| 电脑48000Hz输出 | 97.78% | 94.30% | 166442 B/s |
| 单逻辑CPU调度对照 | 90.82% | 90.86% | 160381 B/s |

名义I2S供样176468 B/s。44.1k环缓冲337756→47020字节，48k环缓冲220488→3488字节。初段平均值掩盖稳态缺额；不能凭整段≥97%判通过。单CPU亲和性已恢复65535，不采用。

同曲Call the shots有限6图：UTC09:57:56.054到09:58:06.575，进度0:38、0:41、0:44、0:45、0:48、0:49。没捕获倒退，不代表倒退不存在。原无音频旧版没有重建比较，用户的历史退化报告仍待定位；当前同版关闭输出的结果不能抹掉这一报告。

完整证据：`R128-RESULT.json`、三个`r128-*-60s.json`、`r128-affinity-60s.json`、`*-qemu.log`、`*-events.jsonl`、`inspect-endpoint.json`、`affinity-restored.json`、`formal-hashes.json`。

- 已完成：电脑音频输出接通；缓存逐采样环境变量等 R122 优化；R125 成对 warp/bias 账本核对；R126 非空闲时间归因；R127 中断来源观察及三个候选试验。
- 用户实测：逐轮改善，但卡顿仍存在。正常音调、慢放/撕裂；iPod 系统也卡；进入 **iPod 内 Vortex** 曾改善。用户新增关键证据：接入电脑音频以前没有卡顿和进度倒跳；现在可能 0:11→0:13→0:12。
- 进行中：把新增的电脑音频链路作为退化排查重点；严格保持相同计时与解码配置进行 A/B。
- 尚未完成：稳态性能目标、无倒跳与听感验收、Cover Flow/设置回归、完整自然休眠普通键唤醒回归。

### 已有结论的边界

R126：播放 V/W 约 0.94；exec 约4.5%、gap约2%；~960/s短非空闲脉冲。成对 warp/bias 健康，不能重新声称 bias 欠推进。sleep=off 已测虚拟时间狂奔、UI冻结，禁止采用。

R127（历史样本，非当前验收）：

| 候选 | 结果 |
|---|---|
| VIC相同电平合并 | 60s V/W 0.95824，未达标 |
| VIC应答/优先级输出修正 | 组件测试通过，实际播放 V/W 0.93754，未达标 |
| halted IRQ kick优化 | 初段60s V/W 1.00627，但后续回落约0.94；不能算成功 |

原生观察定位到 TimerB IRQ8、6000计数/6MHz，即固件真实1kHz定时器。不能把真实 tick 简单丢弃或降频。R127候选全部独立开关，默认关闭；当前R128不启用这些优化。

## 3. 路径、仓库和技术栈

- 项目 `K:\CodexResources\2026-10-28\ipod-classic7-desktop`
- 接续索引 `CONTINUE-HERE.md`
- 旧交接 `K:\CodexResources\2026-11-01\ipod-classic7-handoff\R126-MIGRATION-TO-CODEX.md`
- R123–R127证据 `acceptance\audio-pacing-20261101`
- 本轮证据 `acceptance\audio-output-20261116`
- 最初引用的模拟器项目：<https://github.com/davidmonterocrespo24/qemu-ipod-classic>
- 旧交接记录的上游：<https://github.com/devos50/qemu-ios>；硬件参考 <https://github.com/freemyipod/qemu>。本轮未联网重新核实仓库当前状态。
- 本地项目对应fork：<https://github.com/gamehhh7/qemu-ipod-classic>，K1/K2仪器PR #2/#3。
- QEMU源码 `K:\CodexResources\2026-09-30\ipod-emulation\qemu-fc1307\src\qemu-ipod-classic-1c33be87c7e97b56053142ae9d7f0686feaed504`
- 构建目录 同级 `qemu-fc1307\build`
- 技术栈：QEMU/ARM/TCG C，MinGW64；C# WinForms前端 `Classic7.cs`；Python持久存储后端/SQLite COW；DirectSound音频；RFB显示；QMP控制。
- 正式UI `bin\iPodClassic7.exe` SHA256 `DE2A8E1D6D9473A19ACC26D6070C7941F7CCB3E68563526B6F968F4AAFD6EEA6`
- 正式引擎 `engine\qemu-system-arm-classic7.exe` SHA256 `B55E89D6C4D33E8430F32D3A8D0F05FE8D20165C764D2AE6F00498528DB0549D`
- R127候选 `engine\qemu-system-arm-classic7.audio.exe` SHA256 `EFB2EFE76B372BA9FFE90D7C27C837F5DA42998F5B189D82438E07F0392C4784`
- R128候选 `engine\qemu-system-arm-classic7.r128.exe` SHA256 `2E59DD71CE970C9B6057C21D5AB564C24EAC7894705751DCBCC05748E7110365`
- R128侧车 `bin\iPodClassic7.r128-ab.exe` SHA256 `1AD9E22C4EB47EECBE07B420FEABFD033627645B19ADB8BAC7BFDD832DD3F3C6`

## 4. 未完成任务（优先级）

1. 在原生IRQ分发/BQL交接处做有界归因；真实1kHz tick不能删除或伪造。上一轮低成本电平合并和本轮调度限制未解决。不得再盲试sleep=off、优先级或更大缓冲。
2. 定位用户进度倒跳：区分guest进度源、RTC/虚拟计时及LCD描述符/帧交换。优先找未接音频旧版的精确可重建基线，不声称已复现。44.1k与48k对照已完成：本轮GetMixFormat读到 HECATE G2 GAMING HEADSET、48000Hz、双声道、32bit混音格式。32bit是混音格式，不是已验证硬件DAC位深。仅对齐采样率无效。
3. 若输出链路确实导致退化，定位重采样、DirectSound缓冲和持锁耗时；只在证据支持处修复，禁止靠调大缓冲掩盖持续供样不足。
4. 稳态连续播放 V/W≥0.97，exec占墙钟<1.5%；无进度倒跳、音频持续正常、导航正常，再做Cover Flow/歌曲设置/自然睡眠验收。
5. 测量结束正常关闭侧车，核实唯一写入者退出，回到正式程序；不得把未验收候选覆盖正式版。

## 5. 约束与坑点

- 不用Cursor云额度，不用Firecrawl，不派技术子代理。主代理负责技术；现有sleep_visual（Luna low）只做UI。
- 单K盘COW写入者；不碰实体盘、固件；不伪造IRQ/GPIO/guest RAM/PC，不禁用sleep或reset。保留种子和唯一原始回滚包。
- 同步时禁止Hold/关闭。旧PID/截图/窗口句柄无效；每次核进程创建时间、路径、argv、hash和实际窗口。
- 默认固定 `-icount shift=3,align=off,sleep=on`。此前auto未通过，不采用；sleep=off明确失败。
- 正常关闭，不能连续Hold或强杀用户会话；每次UI动作后至少2秒再观察。菜单M/Up，Down是播放/暂停，不是下移菜单。
- GDB有界且detach；禁止inferior函数调用。ReadProcessMemory采样无挂起、无写入；native地址必须匹配新PID。
- AUDIOLIVE原来同时控制Windows1ms计时请求，所以直接关闭会引入变量；R128仅在测试源把1ms请求独立于输出保留，原始源和正式引擎不变。
- 2026-11-16首次误以Hidden启动测试UI，随后仅恢复该PID的隐藏窗口；后续交互程序必须正常显示。不能把无可见窗口或FPS0当成死机。
- 实际额度接口本次仅返回周额度（已用4%，剩96%）；五小时字段未知，不可按0使用推断。

## 6. 下一步具体第一步

读取本目录 `R128-RESULT.json`、`ROLLBACK.json` 并核当前进程；预期已恢复正式版，不另开第二个写入者。下一刀是实际IRQ分发/BQL交接或显示倒跳归因，不重复上述失败对照。用户指出的历史版本行为仍需精确旧基线比较。

## 7. 完整可贴的检查配置

```powershell
$project = 'K:\CodexResources\2026-10-28\ipod-classic7-desktop'
Get-CimInstance Win32_Process |
  Where-Object { $_.Name -match 'iPodClassic|qemu-system|python' } |
  Select-Object ProcessId,ParentProcessId,CreationDate,ExecutablePath,CommandLine
Get-FileHash "$project\bin\iPodClassic7.exe", "$project\engine\qemu-system-arm-classic7.exe"
```

以下仅设置测试启动进程的环境；**不包含启动动作**，启动前必须确认没有写入者。

```powershell
$env:CLASSIC7_ICOUNT_SHIFT = '3'
$env:CLASSIC7_ICOUNT_GAP = '1'
$env:CLASSIC7_ICOUNT_WARP_EFF = '1'
$env:IPOD_AUDIO_TIMING = '1'
$env:CLASSIC7_AUDIO_LIVE = '0'  # A=0静音输出；B=1电脑输出
$env:CLASSIC7_HOST_RATE = '44100' # C=48000，只改电脑输出格式
foreach ($name in @('CLASSIC7_ICOUNT_SLEEP','CLASSIC7_ICOUNT_WAKE',
 'CLASSIC7_VIC_COALESCE','CLASSIC7_VIC_PRIORITY','CLASSIC7_IDLE_KICK_FAST')) {
  [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
```

完整候选源码与重建辅助：本目录 `r128-i2s.c`、`Classic7.r128-ab.cs`、`prepare-ab.py`、`r128.rsp`；只用候选输出路径。`inspect-endpoint.py`调用只读GetMixFormat，不改系统设置。全部验收结果以实际新记录补充，不能把编译成功等同修复完成。
