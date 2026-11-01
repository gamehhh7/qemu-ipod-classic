# iPod Classic 7 桌面版

双击 `bin/iPodClassic7.exe` 启动。它是原生 Windows EXE，不需要浏览器。

## 操作

- 在触控滚轮上沿圆周拖动：滚动固件菜单。鼠标滚轮也可滚动。
- 按滚轮上方 MENU、下方播放/暂停、左右上一首/下一首；这四个区域都在同一块滚轮上。
- 中央圆键：确认；外圈长按会持续按住对应硬件按键。
- 顶部 Hold 开关：实际驱动模拟器的 Hold GPIO，固件可显示锁图标；锁定时禁止滚轮和按键输入。
- USB 按钮：切换模拟器 PMU 的真实接线输入，并回读接线状态。
- 关闭窗口：关闭本窗口启动的模拟器和存储后端。

键盘：Enter 确认，M/↑ 为 MENU，空格/↓ 为播放暂停，←/→ 为上一首/下一首，H 切换 Hold。

## 屏幕与资源

屏幕通过仅绑定本机回环地址的 VNC/RFB 读取 QEMU 的实时 320×240 LCD。屏幕内容来自原始固件，前端不重绘固件菜单。

本机版本使用现有 `K:\CodexResources` 下的模拟器运行依赖、OS 2.0.4 payload 和已有种子介质；因此它目前是本机运行版本，不是脱离这些资源的便携包。原正式 FCa0c9 和固件未修改；种子介质只读，运行时写入进入独立持久化覆盖层。

2026-10-29 更新：LCD 以原生320×240、整数倍率呈现，启用DPI感知；已实际拖动窗口缩放验证。显示端移除了35ms额外等待，合并重绘并缓存机身绘制。相同60Hz合成RFB输入实测由27.8提升到60.3次更新/秒；这不代表所有固件或游戏都达到60FPS。

USB 按钮现会尝试原生主机控制交互，数据来自固件实际DMA，连接条件在控制器内检查。部分首次接线场景已真实显示并保持Connected，但普通热插拔和重复连接仍不稳定，不能视为全部修好。Windows虚拟iPod磁盘与实际文件读写/重连持久化已完成，iTunes/Apple设备App同步仍在推进。

虚拟磁盘写入保存在`userdata/windows-disk.json`选择的持久化覆盖层中，当前为`userdata/windows-8g.sqlite3`；原种子仍只读，只有一个后端可写该覆盖层。已验证重启后扇区数据保留。已配置的覆盖层缺失时会拒绝启动，避免误建空盘。不要在运行时删除覆盖层或把它同时挂载给其他程序。

源代码：`Classic7.cs`。隔离的输入扩展与构建：`build_engine.py`、`src/wheel_frontend.c`。验证日志和真实 LCD 截图位于 `runs/`、`acceptance/`。

当前主程序从本项目的engine和host目录加载已合并实现，历史实验目录不再作为运行入口。最新实际结果见acceptance/CURRENT-RESULT.json。

2026-10-30：性能改动直接合入本项目。日常外设时钟按模拟CPU指令推进，保留环境变量CLASSIC7_DETERMINISTIC=0作宿主时钟诊断；高频诊断日志可用CLASSIC7_DIAGNOSTICS=1选择。真实固件执行、ATA512到USB4096映射和WAL FULL持久化保持。覆盖层扇区数量按事务维护，回滚恢复原计数，避免每条磁盘命令反复全表计数。仅保留一份回滚包backup/previous-release.zip。最新实际验收状态见acceptance/CURRENT-RESULT.json。

2026-10-30 挂载修复：8GiB盘的主、备FAT32引导扇区已补齐原有种子使用的保留区`FAT`卷分类标记，各改3字节，文件位置和磁盘格式保持。此前缺失此标记，固件在读取FAT/目录前拒绝数据卷。实际成品及完全重开后的新后端均读到音乐，About显示1首歌曲、44.3MB已用、7.9GB空闲。语言修改后的再次保存和Apple设备App同步仍在验收，最新状态见`acceptance/CURRENT-RESULT.json`。不要重建或格式化现有磁盘。

Cover Flow电源提示已修复并完成普通默认成品验收：I2C新地址阶段清除旧完成状态，STOP按原BUSY状态通知从设备结束传输；默认恢复按模拟CPU执行进度推进外设时间，避免繁忙画面时I2C字节在固件中断尾部过早完成并重复传输。普通默认版本4次实际进入Cover Flow均无电源提示或重启；独立电源日志运行42次电池采样全为正常值，错误ADC写入和重启均为0。保留正常的低电量处理和软件重启功能。记录位于acceptance/power-coverflow-20261030/RESULT.json，被动电源诊断日志可用CLASSIC7_POWER_TRACE=1选择。


2026-10-29 R91 合并：此目录是唯一维护入口。USB引擎位于engine，原生控制/数据包/Windows USBIP位于host，当前设备源位于src/usb和src/include。运行不依赖旧实验分支。普通连接由本程序调用官方USBip工具导入，直到按断开或关闭窗口；不再调用定时detach观察脚本。每次接线有独立的本机端口和日志，错误不自动重试。不改变驱动或物理设备。Windows已实证USBSTOR和Apple iPod USB Device磁盘，文件读写/重连和同步仍待完整程序实际验证。


当前默认介质为userdata/windows-8g.sqlite3：8GiB FAT32，4096字节逻辑扇区/32KiB簇，ATA512到USB4096的映射由固件完成。原disk.sqlite3与原始种子均保留。Windows实际写入、刷新、回读及完全新后端重连后同一文件哈希一致。不要重复运行create_windows_disk.py，它只用于首次创建专用盘。

普通双击运行无自动退出；--connect-usb表示开机即接线，同样没有退出倒计时。--max-seconds仅用于显式限定的诊断运行。设备身份位于userdata/device-identity.json，固件通过原有启动交接结构读取，不重绘界面或伪造USB返回。当前已配置MC293LL、虚拟序列号8K0090019ZS以及教程公开FwId对应GUID。正常同步尚未验收，恢复流程尚未实现，请勿点恢复iPod。
