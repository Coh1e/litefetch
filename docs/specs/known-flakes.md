# 已知偶发失败

一条命令验收（`test-local.cmd`）的前提是它稳定。这份清单记录**已知会偶发**的测试、实测频率、
已做的缓解与下一步——避免每次重新调查，也避免把偶发当成"这次改动弄坏的"。

频率都是在本机（非干净 Windows、长期有历史进程与 DSH 会话）实测。

| 测试 | 症状 | 实测频率 | 已做的处理 | 下一步 |
|---|---|---|---|---|
| `core-bt` | 早前：`seeder listens on a loopback port` / 端口报 0 / 30 s 没传完。**根因已定位**：Windows 保留整段 UDP 端口（`netsh int ipv4 show excludedportrange protocol=udp`），libtorrent 要在同一号上同时监听 TCP 与 uTP，落在保留段就 `[sock_bind] [uTP] access denied` | 修复前 8 轮 1 次、6 轮 1 次；修复后 **10 轮 1 次（且为 120 s 挂死而非断言）** | 预留端口时同时探测 TCP 与 UDP；两处端口检查有界等待并读 listen alert；失败信息带 `peers/wanted/done/rate`；`core-bt` 的 TIMEOUT 提到 180 s（挂死护栏） | 那一次挂死未定位。**已加阶段日志**：`%TEMP%\core-bt-trace.log` 记录 `seeder started / leech loop entered / stopping seeder / seeder joined`，挂死时最后一次被杀进程写下的是哪一行就是卡点。之后 12 轮全绿（2026-10-30） |

| `ui-window` | `download did not publish`（30 s 内文件没落地） | 2026-10-28 约 1/3；**2026-10-30 共 19 次整套运行 0 次** | 选行落在别的任务上的竞态已修；超时信息补已写/期望字节与 worker 任务状态 | 已按此结论关闭 #32（`.pi/evidence/32/`）；再出现时看 `task=` 与 `wrote=/expected=` 直接重开 |

## 遇到偶发时的做法

1. 先看失败信息里的状态字段，再决定是重跑还是调查；
2. 重跑只用于确认是不是已知偶发，不用于把红的说成绿的；
3. 连续两次同一测试失败 → 当作真问题处理，不重跑；
4. 新的偶发（症状不在上表）→ 补一行记录，并在 Issue 里留证据路径。

## 相关

- `docs/research/bt-test-flakiness.md`：逐轮的原始记录与假设；
- `docs/specs/verification-index.md`：测试与票面的对应；
- 证据：`.pi/evidence/core-bt-flake/`、`.pi/evidence/32/`。
