# TCP 建连的停止、期限与工作线程事件分发

2026-10-20，新增 `downloader/src/socket_connect_wait.*`，PeerConnection 的 transport/Hello 建连都使用该机制。不增加库、线程或独立进程。

在拥有socket的原工作线程内运行Qt事件等待，stateChanged/errorOccurred唤醒循环，定时器最多100ms检查一次停止和原有QDeadlineTimer总预算。取消、错误或期限到期统一abort连接；连接完成与停止同时发生时，先检查停止，不向已经取消的连接发送后续握手。使用ExcludeUserInputEvents，不在GUI线程运行网络工作。

首次全量回归暴露工作线程问题：受控pending-state测试通过，但原Kad统一任务场景失败，日志 logs/downloader-tests-connect-cancellation.log、connect-kad-repro.log 均为TCP fixture read failed。按 diagnosing-bugs 技能缩小为新std::jthread中仅connectTransport+发送一帧，logs/connect-worker-red.log 在1.08秒稳定失败：TCP connection deadline expired。

检查本地Qt6.11.2源码：QNativeSocketEngine只有threadData已有eventDispatcher时才注册连接通知，QEventLoop构造才ensureEventDispatcher。原顺序先connectToHost后创建事件循环，新std::jthread没有dispatcher，OS连接可能到达对端而Qt状态无法推进。修正为先创建QEventLoop确保dispatcher，再发起异步连接。没有改换线程架构或回退不可取消的长waitForConnected。

修复后最小实际线程测试连续3次通过（logs/connect-worker-green.log，0.16秒），原Kad→TCP任务测试也连续3次通过（logs/connect-kad-fixed.log，3.50秒）。没有新增临时debug日志或改变默认重试次数掩盖问题。

测试 socket_connect_wait_test.cpp 以受控QTcpSocket状态/Qt定时器验证：延迟200ms的成功、30秒期限内取消、120ms整体期限、连接拒绝、同时完成/取消以及预先取消，失败均必须回到UnconnectedState且不等满长超时。socket_connect_worker_test.cpp 则使用两个真实工作线程和回环TCP，独立检查新线程能够完成连接并发送已知帧；原有服务器/Kad统一任务继续验证真实调用点。

构建 logs/downloader-build-connect-cancellation.log 成功。受控pending阶段测试通过（logs/socket-connect-cancellation-tests.log，1/1，0.69秒）。该测试不通过修改防火墙制造真实网络黑洞，因此pending取消/期限的确定性状态测试与真实TCP成功路径证据需分别理解。

本阶段更新此前TCP建连必须等待完整超时的限制。当前仍不能抢占内核中的单次磁盘IO/flush/rename；Kad长期节点维护、真实公网兼容、回调/混淆等完整目标仍未完成。
最终全量回归 logs/downloader-tests-connect-cancellation-final.log：32/32通过（49.74秒），覆盖真实工作线程建连、取消、Kad/ed2k、缓存/发布恢复、Qt设置、HTTP/BT及浏览器桥接。
