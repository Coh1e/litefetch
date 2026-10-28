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
- **本次选择**：进程内驱动 + 发布前一次人工 smoke。

## Consequences

- 真启动才证得明的东西只剩入口胶水（`wWinMain` 的参数解析、控件初始化、消息循环、
  图标/资源/清单）；参数解析已抽成可测函数，其余交给 #28 的那一次真启动。
- 桌面专属行为（真实 Explorer 窗口、跨进程读另一个进程的 ListView、系统剪贴板的跨
  进程语义）不再被验证；"打开文件夹"改为断言要执行的命令行，不再真的启动资源管理器。
- 证据 PNG 由 `ui-window` 自己 `PrintWindow` 产出（见 `LITEFETCH_EVIDENCE_DIR`），不
  再依赖外部脚本截图。
