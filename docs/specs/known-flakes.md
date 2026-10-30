# 已知偶发失败

一条命令验收（`test-local.cmd`）的前提是它稳定。这份清单记录**已知会偶发**的测试、实测频率、
已做的缓解与下一步——避免每次重新调查，也避免把偶发当成"这次改动弄坏的"。

频率都是在本机（非干净 Windows、长期有历史进程与 DSH 会话）实测。

| 测试 | 症状 | 实测频率 | 已做的处理 | 下一步 |
|---|---|---|---|---|
| `core-bt` | `seeder listens on a loopback port` / `the seeding session reported its listen port` / `a second peer downloaded the torrent from us` / 偶发 120 s 超时 | 8 轮 1 次、6 轮 1 次、6 轮 1 次（2026-10-30） | 种子会话先预留回环端口再交给 libtorrent；两处端口检查改为有界等待并读取 `listen_failed_alert` 的原因；失败信息带 `peers/wanted/done/rate` | 怀疑预留端口与 leech 会话的临时绑定互相抢；下一步把 leech 会话的端口也固定，或在失败信息里加当时的监听套接字列表 |
| `core-bt`（早前） | `seeder listens on a loopback port` 5.9 s 失败 | 偶发 | 端口从 5 s 有界等待改为 15 s，并改读 alert | 同上 |
| `ui-window` | `download did not publish`（30 s 内文件没落地） | 2026-10-28 约 1/3；2026-10-30 连续 12 次整套运行 0 次 | 选行落在别的任务上的竞态已修（确认目标任务离开"等待中"，否则重选一次）；超时信息补已写/期望字节与 worker 任务状态 | 复现时看失败信息里的 `task=` 与 `wrote=/expected=`，区分"没启动"与"peer 卡住" |

## 遇到偶发时的做法

1. 先看失败信息里的状态字段，再决定是重跑还是调查；
2. 重跑只用于确认是不是已知偶发，不用于把红的说成绿的；
3. 连续两次同一测试失败 → 当作真问题处理，不重跑；
4. 新的偶发（症状不在上表）→ 补一行记录，并在 Issue 里留证据路径。

## 相关

- `docs/research/bt-test-flakiness.md`：逐轮的原始记录与假设；
- `docs/specs/verification-index.md`：测试与票面的对应；
- 证据：`.pi/evidence/core-bt-flake/`、`.pi/evidence/32/`。
