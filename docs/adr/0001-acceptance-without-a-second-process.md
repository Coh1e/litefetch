# 验收不启动第二个进程

Status: accepted (2026-10-28)

一张票的验收只跑 `test-local.cmd`（ctest），不启动已构建的 GUI 可执行文件；
`ui-window` 在测试进程内创建真实 `MainWindow` 并用 `SendMessage` 驱动它，因此
窗口行为照样是"真窗口"证据，但不再需要交互式桌面、不再抢前台窗口、也不再触发
Windows 对未签名程序的确认提示。`scripts/win32-smoke.ps1` 保留为**发布前一次性**
检查，归 #28（全范围总验收与干净 Windows 交付验证）执行，不在每票验收里跑。

## Considered Options

- **每个 ticket 跑桌面 e2e（原方案）**：687 行的 `scripts/win32-e2e.ps1` 用
  `SetForegroundWindow`、`PrintWindow`、跨进程 `ReadProcessMemory` 驱动真窗口，还要
  拉出真实 Explorer、读写系统剪贴板。它只能在人类已登录、无遮挡的桌面上跑，每跑一次
  都可能弹一次 Windows 确认，等于把验收变成人工流程。
- **完全不要真进程证据**：省事，但"exe 能链接、能起来"就没人证过。
- **本次选择**：进程内驱动 + 发布前一次真启动 smoke。

## 修订：2026-10-28（#30 定位、#31 落地）

本文第 8 行原写"发布前一次性检查"，当时默认它会卡在未签名的 Windows 确认框上、
必须有人点一次。**这个前提已不成立**：#30 的定位表明那个确认来自 ShellExecute 的
文件信任检查（未签名才会触发，已签名的不会），改用 CreateProcess
（`UseShellExecute = false`）即可完全绕开；#31 把 `scripts/win32-smoke.ps1` 改成这条
路径，并加了"窗口出现/进程退出"两个时间上限，超时以非零退出并打印当时可见的
`#32770` 对话框。因此该 smoke 现在是**无人值守**的，不再需要人在场。

不变的部分：它仍然只在发布前跑一次（归 #28），仍然不进入每票验收。用户双击 exe
走的是 shell，仍会看到一次"未知发布者"，那只能由签名解决（#27），不属于本 ADR。

## 修订：2026-10-31（#34）

托盘图标、右键菜单外观和通知气球只能由真实通知区观察，`ui-window` 无法在无通知区的会话里断言它们。
#34 的自动验收仍只跑 `test-local.cmd`；真实桌面部分按
[人工验收清单](../specs/human-acceptance-checklist.md) 第 6 节由人确认，不启动第二个自动化进程，
也不替代自动验收。

## Consequences

- 真启动才证得明的东西只剩入口胶水（`wWinMain` 的参数解析、控件初始化、消息循环、
  图标/资源/清单）；参数解析已抽成可测函数，其余交给 #28 的那一次真启动。
- 桌面专属行为（真实 Explorer 窗口、跨进程读另一个进程的 ListView、系统剪贴板的跨
  进程语义）不再被验证；"打开文件夹"改为断言要执行的命令行，不再真的启动资源管理器。
- 证据 PNG 由 `ui-window` 自己 `PrintWindow` 产出（见 `LITEFETCH_EVIDENCE_DIR`），不
  再依赖外部脚本截图。
