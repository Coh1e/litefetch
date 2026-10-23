# aMule 后台生命周期

2026-10-20。新增 downloader/src/amule_runtime.h/.cpp，CMake download-ed2k 纳入该模块。

AmuleRuntime 在专用 std::jthread 内持有 QProcess，不跨线程操作 QProcess。启动阻塞，因此后续统一任务适配必须在工作线程创建。每次运行产生新的随机密码和两个临时本机端口；端口先通过 loopback socket 选取，释放后启动后台。端口存在短暂竞争窗口，启动健康检查失败时报告错误并关闭自建进程，不声称操作系统提供了原子端口移交。

状态目录规范化后持有 QLockFile，拒绝重复所有者。HOME/USERPROFILE/APPDATA/LOCALAPPDATA/TEMP/TMP 仅对后台子进程指向状态目录下的隔离路径。配置、incoming、parts及日志均在指定状态目录。默认关闭公网自动连接、ed2k/Kad、UPnP和更新。此阶段不注册协议、不加防火墙规则、不加载迅雷DLL。

真实认证与 EC 健康检查成功才返回连接配置。析构请求 supervisor 停止，使用 amulecmd shutdown 让核心保存状态，等待后仅对本实例持有的 QProcess 强制收尾；不按进程名结束别人的后台。启动失败也清理自建进程。状态所有权锁保留到异常清理之后。

## 测试

运行 downloader/test-local.cmd --amule-runtime，实际启动项目内 aMule 3.1.0 Windows 二进制与原生 API 客户端。验证启动就绪、同状态目录第二实例被拒绝且原实例仍可用、添加离线任务、暂停、析构后 API 端口关闭、重启凭据改变、暂停任务从磁盘恢复为 stopped/paused 且大小/进度不变。新建成功测试目录自动清理；异常时保留现场。构建日志 logs/downloader-build-amule-runtime.log，最新实测 logs/amule-runtime-live.log，完整通过约2.5秒。

最初测试第二次启动报 Cannot save aMule configuration，失败目录 amule-runtime-YNxskW 中已有首次后台保存文件。Qt QSettings 读入 wxWidgets 写回格式后不能完成配置同步；用保留原始行、只更新明确管理字段的 QSaveFile 写入替代，重复启动通过。已有键原地替换、重复托管键移除，不逐次追加重复配置段；不改其他用户设置。

## 仍缺

- 尚未接入 TaskManager 和 GUI，也未启用公网发现或进行实际 ed2k 文件传输。
- 整个父应用被强杀后的孤儿进程回收尚未实现，Windows Job Object 等方案仍需验证。当前证明正常析构与可捕获的启动失败清理。
- Windows状态目录凭据ACL、旧凭据清理、API掉线后用户提示/重启策略与实际端口竞争处理仍需完善。上游密码初始化和关机命令使用其密码命令行参数，不能声称凭据对同用户进程列表不可见。
- 后台全局配置与用户上传/下载策略、实际文件完成发布、服务器/Kad、源码构建及分发许可仍按总范围继续。
