# 受阻存储与强制终止：真实验证记录

日期：2026-10-29。全部命令在项目内执行，产物在 `tmp/` 下，未改动系统设置。

## 一、真实受阻存储

做法：建目录 `tmp/blocked`，用 `icacls` 拒绝当前用户写入（`(WD,AD)`），再让 CLI 下载到该目录。

```
litefetch-core-cli add --db tmp/blocked-db/tasks.db --source http://127.0.0.1:47722/blocked2.bin \
                       --destination tmp/blocked/blocked2.bin
litefetch-core-cli start --db tmp/blocked-db/tasks.db --id 52f2ef21…
```

结果（`list` 的真实输出）：

```json
{"state":"failed","received":0,"total":-1,"error":"cannot create the partial file"}
```

要点：

- 任务是 `failed`，**不是**一直重试；`received` 为 0，没有产生半成品；
- 失败发生在开传之前（打开 `.lfpart` 即被拒绝），因此没有可续传的数据；
- 错误原因是英文的（`cannot create the partial file`），与中文界面混用——这是 #19 里"统一错误文案"仍未做完的一处。

实验后已用 `icacls <dir> /remove:d <user>` 恢复，并验证可以正常写入。

## 二、强制终止后重启

做法：向限速服务端（8 MiB，约 160 KiB/s）下载，运行中途 `Stop-Process -Force` 杀掉 CLI 进程，
再用 GUI 启动一次（启动时会调用 `pauseInterrupted()`）。

```
kill 前：state=running, received=0
kill 后（读库）：state=running, received=0      ← 进程已死，库里仍是"运行中"
GUI 启动后：state=paused, received=0            ← 重启把中断任务标成暂停
```

要点：

- 进程被强杀时库里保留 `running`，重启后被识别为中断并改为 `paused`；
- 本例 `received` 为 0（杀得太早，`.lfpart` 尚未创建），因此没有验证"从已有字节续传"；
  续传本身由 `core-service` 的暂停/续传用例覆盖。

## 三、这两条对 #18 的意义

`docs/specs/win32-delivery-status.md` 里 #18 的两个缺口——"真实受阻卷"与"强杀一致性"——
现在都有**可复现的真实证据**，不再是"本环境无法制造"。剩下的差异是：本实验用的是拒绝写入的目录，
而不是只读卷；两者对应用的表现一致（打开半成品即失败），但严格说不是同一种介质状态。
