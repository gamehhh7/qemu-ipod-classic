# Classic7 Desktop Upload Notes / 上传说明

## English

- Based on upstream/fork qemu-ipod-classic @ commit `1c33be87c7e97b56053142ae9d7f0686feaed504` (same as local source marker).
- Target GitHub: branch of `gamehhh7/qemu-ipod-classic` (fork of `davidmonterocrespo24/qemu-ipod-classic`).
- This folder is the **Windows Classic7 desktop overlay** and related **device-model / engine build patches** (source + docs), not a full QEMU tree dump with payloads.
- **Excluded on purpose:** firmware payloads, media/seed `.img`, userdata COW / sqlite disks, built `.exe` binaries, `.obj`/`.d` build objects, previous-release archives, large run logs, secrets, `.env`.
- **Audio status:** still unfinished (slow / tear). Work was paused by the user.

## 中文

- 基于 qemu-ipod-classic 提交 `1c33be87c7e97b56053142ae9d7f0686feaed504`（与本地源码标记一致）。
- 计划上传到 `gamehhh7/qemu-ipod-classic` 的分支（上游 fork 自 `davidmonterocrespo24/qemu-ipod-classic`）。
- 本目录是 **Windows Classic7 桌面壳层** 与相关 **设备模型 / 引擎构建补丁**（源码与文档），不包含固件与镜像载荷。
- **刻意排除：** 固件 payload、媒体/seed `.img`、userdata COW/sqlite、已编译 `.exe`、`.obj`/`.d`、旧版 zip、大型运行日志、密钥与 `.env`。
- **音频状态：** 仍未完成（偏慢/撕裂），用户已暂停相关工作。

## Rebuild hints

- See `README.md`, `CONTINUE-HERE.md`, `WORK-STATE.json`, and `engine/build-recipe.json` / `*.rsp`.
- Do not expect bundled firmware or disk images in this package.
