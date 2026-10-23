# 浏览器恢复队列与真实 Chrome 联测准备

2026-10-20。

恢复扫描此前会被一条损坏记录阻塞。storageJournal 现在提供 scan，报告无效条数并返回有效记录；worker 对每条恢复分别处理异常，保留损坏数据并显示提示，继续恢复其他任务。原有严格 list 接口保留。新增两项回归用例先失败后通过，浏览器与配置测试共 63 项通过，见 logs/browser-recovery-isolation-before.log 和 logs/browser-recovery-isolation-after.log。9 文件开发扩展包已重新生成。

TaskManager 创建浏览器准备事务时使用 120 秒期限，为每分钟恢复检查提供窗口。过期仍拒绝提交；浏览器延迟调度超过期限时不能保证自动恢复。最终构建成功，download-handoff-store 通过，包含 61 秒后查询仍 Prepared 并提交及重复提交不重复派发的验证。日志：logs/downloader-build-recovery-final.log、logs/downloader-test-recovery-final.log。第一次测试筛选误用了不存在的 browser_handoff 名，CTest 明确报无测试；已纠正为实际用例名，未把空结果计为通过。

用户已明确允许临时新增 Chrome 的单个 HKCU NativeMessagingHosts/org.native_downloader.bridge 注册项，测试后恢复。此授权不包含 Edge、HKLM 或永久注册。scripts/with-chrome-native-registration.ps1 封装重新检查 32/64 位基线、拒绝已有注册、记录项目内审计文件及 finally 清理；若测试期间出现外来值则保留并报错。该脚本尚未执行，不把实现当作注册/恢复验证结果。

Chrome 155 正式版在项目内隔离资料目录启动探测，尚未进入扩展加载阶段：Playwright 启动超时，Chrome 报 remote debugging requires a non-default data directory，尽管命令行已含项目内 --user-data-dir。已核对并终止本次探测的独立主进程，未按进程名称批量结束其他浏览器。没有修改注册表。结果保存在 downloader/out/verification/browser-real-profile/chrome-probe.json；错误原因尚待定位。

官方资料确认 Chrome 137 起正式版移除 --load-extension，而 Chrome for Testing 保留：[Chromium Extensions 公告](https://groups.google.com/a/chromium.org/g/chromium-extensions/c/1-g8EFx2BBY/m/S0ET5wPjCAAJ)。当前另有官方扩展生命周期测试工具入口：[Chrome DevTools 扩展测试](https://developer.chrome.com/docs/devtools/agents/extensions)。本地探测拟使用 Extensions.loadUnpacked，仍需先解决隔离浏览器的调试连接，不能据此声称真实自动接管已通过。
