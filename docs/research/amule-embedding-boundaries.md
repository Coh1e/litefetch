# aMule 3.1.0 内嵌验证边界

后续约束：用户明确不引入wxWidgets，UI唯一Qt。因此本实验已停止，两个准备/构建脚本入口已禁用。恢复产品manifest时vcpkg已移除9个实验包；最初库存清单误计purge历史记录，已通过install ok installed状态过滤修正。保留本文作为研究证据，不将此路线推进为产品依赖。

2026-10-20。继续用户要求的库内嵌比较，后台路线不因此成为已锁定架构。

当前实读源码结论：

- src/amuled.cpp:116 已使用 IMPLEMENT_APP_NO_MAIN(CamuleDaemonApp)，独立main在后面调用wxEntry。因此可以从分离入口开始，不能声称必须先重写整个核心。
- 同文件OnRun明确检查AcceptExternalConnections与ECPassword，并在Windows注册控制台退出handler，然后调用wxApp::OnRun。单纯去掉main并不会消除端口/密码要求，内嵌变体要明确替代这部分启动契约。
- CamuleDaemonApp的事件表处理核心定时器、DNS、hash完成、文件完成和预分配完成。OnExit调用ShutDown、释放core_timer、CamuleApp::OnExit。内嵌必须验证这些事件与生命周期，不能只证明链接成功。
- src/LibSocketAsio.cpp已有全局io_context和Asio线程调度。wx仍用于事件、计时及部分EC等网络路径。之前笼统说成wx网络栈需要适配不够准确，不应据此推导整个网络层要重写。
- src/CMakeLists.txt内部STATIC目标不是完整内核：amuled仍直接编译CORE_SOURCES/COMMON_SOURCES。可评估把这些源文件与无main入口封装为私有库，向Qt提供稳定接口；是否需要C ABI取决于是否统一工具链。

当前实验先验证同项目MSVC能否构建未改动的上游无GUI核心，避免一边改入口一边混淆工具链失败。scripts/prepare-amule-embedding.cmd使用既有项目内依赖/缓存目录添加wxWidgets与Crypto++，未改变下载器产品manifest。scripts/build-amule-msvc-probe.cmd配置独立构建目录，只构建amuled，关闭GUI/API/协议注册工具及可选更新特性。它仍是构建基线，不是内嵌成功证明。

尚需实证：完整核心MSVC构建、去main库链接、与Qt同进程启动/事件派发/退出、无EC端口的直接命令接口、任务实际下载和磁盘恢复，以及同进程核心重启是否可支持。不会用最小wx/Qt空循环来替代完整aMule内嵌验收。

直接任务控制入口已找到：src/DownloadQueue.h 的 AddLink/AddED2KLink，src/PartFile.h 的 PauseFile/ResumeFile/StopFile。可将操作派发到核心拥有线程后调用这些接口，无需先运行REST服务；但命令线程归属和对象存活仍需真实运行验证。此次构建输入仍是官方3.1.0，.git_archival.txt记录909d304d993ee07df6c6f6acf501a6d791d53666/tag3.1.0。
