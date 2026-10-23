# ed2k/Kad 网络等待中的及时停止

2026-10-20，更新 PeerConnection、ServerConnection、下载协调器和Kad UDP查询/收集/遍历；无新增依赖、独立进程或额外监视线程。

ed2k已有连接的read/write等待按最多100ms切片，片间检查统一的停止回调，同时继续使用原始QDeadlineTimer总预算，正常SocketTimeoutError不冒充连接失败，也不因重试重置总超时。收包缓冲解析前和发送前同样检查停止。server登录/找源及peer握手/分块共享该机制；下载协调器将stop_token及现有TaskManager原子停止标志合并后传入。

Kad bootstrap、lookup、单页源查询、多报文收集、迭代发现和下载provider均传递同一停止检查，UDP等待最多100ms一次。多报文持续涌入时每报文也检查。迭代的网络失败catch分支发现取消时继续抛出，不将其当作普通失败节点后继续查询。没有创建独立轮询线程、后端或强行跨线程操作Qt socket。

测试 network_cancellation_test.cpp 给静默对端设置30000ms预算，确认实际收到网络请求后50ms设置停止标志；Kad bootstrap、findClosest、collectSources、discoverSources、downloadFile和TCP Hello等待6种真实socket场景均要求抛出停止且耗时<1500ms。专项实际通过（logs/network-cancellation-tests.log，1/1，0.77秒）。任务测试把静默服务器预算改为30秒，统一队列pause必须在1.5秒内成为Paused。正常peer测试额外延迟160ms再分片回应，必须跨过轮询窗口仍成功，而不是误判超时。

构建 logs/downloader-build-network-cancellation.log 成功。

限制：TCP连接建立阶段目前仍用一次waitForConnected，只在其前后检查停止，可能等待配置的连接超时；磁盘hash校验/缓存复制和发布阶段也尚非逐块可取消。因此本阶段是已有TCP数据等待与Kad UDP的响应改善，不宣称所有阶段即时取消或整个取消需求已完成。原有完整part缓存和发布恢复语义不变。
全量回归 logs/downloader-tests-network-cancellation.log：29/29通过（48.99秒），包括延迟有效TCP回复、统一任务暂停及HTTP/BT/浏览器原生桥接。
后续阶段：磁盘part/整文件校验、复制、缓存恢复与提交前的停止检查已补充，见 ed2k-disk-cancellation.md；单次内核IO和TCP建连仍不能立即抢占。
后续阶段：TCP建连停止响应已补齐，并修复工作线程事件分发器创建顺序，见 ed2k-connect-cancellation.md；不再使用单次长waitForConnected。
