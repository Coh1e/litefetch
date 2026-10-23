# 真实 Chrome 自动接管与失败回退联测

2026-10-20。使用 Google Chrome for Testing 155.0.8059.84、项目内独立 profile/environment/downloads、实际 MV3 扩展、实际 native-downloader-host.exe、Qt GUI 与 SQLite/libcurl。没有使用浏览器 API 替身。

官方版本元数据保存在 config/chrome-for-testing.json。下载包 tools/chrome-for-testing/chrome-win64.zip 的 SHA256：507f2a031441d10c62b5e3d4e863f09ac1198a571c1419976cbd8387f639afde。这是本地下载完整性记录，不是发布方签名证明。未全局安装浏览器。

## 实际验证

- Extensions.loadUnpacked 加载9文件开发包，浏览器报告固定 ID namdpafnjehflfkcgdbnmoccndohmjoj，enabled=true。证据：downloader/out/verification/browser-real-profile/chrome-testing-probe.json。
- 用户允许的单个 HKCU Chrome host 项临时写入；浏览器真实 sendNativeMessage 查询 missing 成功，证明浏览器注册查找、host 启动、来源验证和 Qt IPC 已接通。
- 通过扩展真实弹窗启用本地站点，然后点击网页下载链接。浏览器先发起请求，扩展完成 prepare/cancel/commit；浏览器下载状态 interrupted / USER_CANCELED，持久 journal 为 committed。原生端恰好一次 GET，落盘4 MiB与服务器内容逐字节一致，SHA256 a117210941a0b00dcb2d8577e680d84b6fa0eaf760d2afc654c953b9859d54fa。
- 关闭扩展接管后，同站点新下载由浏览器完成；仅一个浏览器请求，没有原生请求，文件逐字节一致。
- 重新启用接管并停止原生 Qt 程序后，新下载 prepare 失败，扩展释放暂停状态，journal 为 released；浏览器完成下载，只有一个浏览器请求，内容一致。
- 外层 finally 删除本次新建 host 项；操作审计 restored=true，随后独立读取 Registry32/Registry64 均不存在。没有注册 Edge/HKLM，没有修改个人 Chrome profile。

运行入口：scripts/with-chrome-native-registration.ps1 -TestScript scripts/run-chrome-handoff.ps1。真实脚本 scripts/test-chrome-handoff.mjs。最新日志 logs/chrome-handoff-real.log，机器结果 downloader/out/verification/browser-real-profile/chrome-handoff-result.json，注册审计 downloader/out/package/browser-connection/temporary-registration-result.json。

## 发现与边界

最初正式版 Chrome 的隔离启动被默认目录检查拒绝，原因未定位；同版本 Testing 可正常建立调试连接并装载扩展。本次证明 Chromium/Chrome for Testing 的真实扩展接管，不等于个人正式版 profile 安装、Edge 或 Firefox 验收。使用无账号本地 HTTP GET；登录/Cookie/Referer/POST 等上下文传递仍未实现。

扩展启动探测与联测均为 headless。没有把此测试当作普通用户安装体验、真实公网服务或原版迅雷动态交接验收。原版 socket/prebuffer 证据仍主要来自静态逆向。

扩大联测时发现测试脚本自身清理错误：Windows 子进程被信号停止后 exitCode 仍为 null，finally 二次等待已发生的 exit 导致卡住。已加入 signalCode 检查。卡住的独立测试 Node 经核对后终止，外层 finally 成功清理注册；之后三场景完整重跑通过，耗时约9秒。未把被终止的那次视为通过。

官方参考：[Native Messaging](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging)、[扩展测试入口](https://developer.chrome.com/docs/devtools/agents/extensions)。

## 真实 worker / 原生进程恢复补验

最新脚本已扩大，日志 logs/chrome-handoff-recovery-real.log，机器结果仍为 chrome-handoff-result.json。此前三场景也随之重新通过。

1. 失去提交确认：已真实提交完成的任务，将扩展磁盘 journal 写为 committing，模拟确认未持久化的快照；通过 CDP Target.closeTarget 停止实际 service worker，再创建真实 Chrome alarm 唤醒。新 worker 查询真实 host/Qt，恢复为 committed 且 taskId 不变；重复发送 commit 返回同一任务，无新增 HTTP 请求。同时保留一条 null 损坏记录，确认它没有阻塞有效任务恢复且未被自动删除。
2. 浏览器已取消、原生尚未提交：用实际 chrome.downloads 发起并暂停另一个文件，通过真实 Native Messaging prepare 创建事务，确认此时服务器只有浏览器请求、没有原生 GET；实际 cancel 后确认 USER_CANCELED，写入 cancelled 快照。停止 Qt 进程并重新启动，从新进程查询得到磁盘中同一个 prepared taskId。随后停止并由 alarm 唤醒真实 worker，恢复逻辑执行 commit，最终文件逐字节一致；该文件总共两个请求：被取消的浏览器请求，以及唯一一次原生 GET。

测试有意构造两种持久快照，未声称通过随机时刻杀进程精确复现全部故障窗口。下载 API、alarm、worker 生命周期、Native Messaging、SQLite、原生进程重启及真实 HTTP 文件内容均实际执行；没有改动生产模块或添加生产测试开关。尚未覆盖整个浏览器进程被杀后的下载数据库恢复、120秒超时、cancelling/pausing模糊边界的真人处理流程或登录资源。

最终两分钟内完成，最新执行约13秒。所有本轮临时注册均由 finally 恢复，执行后独立复查 Registry32/Registry64 均为空。
