# ed2k/Kad 同进程方向

最新验证：真实aMule的Kad bootstrap和空结果节点查找已通过，见[kad-real-amule-interop.md](kad-real-amule-interop.md)。首次参考端自动下载节点表的隔离失误及修正也记录在内；公网找源与完整路由能力仍未完成。

2026-10-20用户约束：唯一UI为Qt；不引入wxWidgets；不启动独立ed2k后台；保留libtorrent且严格裁剪库规模；不采用aria2。此前aMule控制/进程管理仅留作对照，默认构建已关闭DOWNLOADER_BUILD_AMULE_EXPERIMENTS，不作为产品依赖。

现有aMule的内部静态库不能直接满足这些约束。后续以QtCore/QtNetwork和现代C++构建同进程协议模块，参考最新稳定aMule源码的协议行为，逐步迁移必要算法/状态机，去掉wx应用和全局GUI/EC控制依赖。不能简单复制整个核心再用兼容层伪装wx，也不在没有完整实测前承诺现成无wx SDK。

已新增src/ed2k_wire.h/.cpp，仅依赖QtCore：TCP协议标记、长度小端编码、操作码、有效负载边界，以及不消费后续包的解码结果。支持标记e3/c5/d4；Packed数据仍保持压缩字节，没有把它冒充已解压；Kad UDP格式不沿用TCP头。对照本地aMule3.1.0的Protocols.h与Packet.cpp核对格式，未复用迅雷二进制。

真实编译通过，download-ed2k-wire测试通过（logs/downloader-build-ed2k-wire.log、logs/ed2k-wire-tests.log）：已知字节向量、每个截断位置、连续包边界、零长度、超大长度、UDP误投、空包及编码限额。该模块还未接入GUI或网络socket，不能当作ed2k下载引擎已实现。

仍需：有界压缩处理、Hello/标签与身份、服务器源发现、peer握手/队列/分块传输、ed2k分块hash与AICH、临时数据恢复、上传策略、Kad发现/路由/持久化，以及真实兼容端到端验证。使用进程内工作线程和消息队列与Qt任务模型交互，线程不是独立后台进程。代码来源与最终开源许可需随实际移植范围核对。

后续进展：基础Hello/HelloAnswer、标签解析及同进程Qt TCP主动握手已实现，受控真实socket专项检查通过，见[ed2k-hello-and-peer.md](ed2k-hello-and-peer.md)。上文“仍需Hello”更新为仍需完整特性协商/入站与真实peer互通，不能将该小阶段当作完整ed2k完成。

后续进展：已加入 ed2k 流式哈希、请求/回复分块、乱序去重落盘与整文件发布校验；受控真实 TCP 的请求批次到最终文件链路验证通过，见 [ed2k-requested-transfer.md](ed2k-requested-transfer.md)。该链路假定已获上传槽，仍需真实队列状态机、找源、Kad、恢复和 GUI 集成。

后续进展：文件名/分 part 状态确认、基础上传队列排名与授槽状态机已加入，并已接到受控 TCP 文件传输测试，见 [ed2k-file-queue.md](ed2k-file-queue.md)。长期队列维护、真实扩展兼容、找源/Kad/恢复及 Qt 任务集成仍待完成。

后续进展：基础服务器 TCP 登录、能力标志、按文件 hash/大小查询及源列表解析已实现，受控真实 TCP 测试通过，见 [ed2k-server-discovery.md](ed2k-server-discovery.md)。尚未把发现源交给下载调度器，Low ID 回调、真实公网互通和 Kad 仍待完成。

后续进展：已加入服务器找源到文件的下载协调器，真实 TCP 连续流程覆盖源不可用回退、跨 part 多批传输、哈希验证和发布，见 [ed2k-download-coordinator.md](ed2k-download-coordinator.md)。当前仅支持直连完整源和 <=4GiB 文件，可信多源 part 调度、真实互通、恢复、Kad 及 GUI 接入仍需继续。

后续进展：已加入 hashset 的文件身份/聚合根验证、真实 TCP 请求，以及磁盘 part 校验与坏 part 单独清除重填基础，见 [ed2k-hashset-and-repair.md](ed2k-hashset-and-repair.md)。跨源复用可信 part 的调度及持久化仍待接入。

后续进展：会话内跨源保留 hashset 已验证 part 已接入协调器，真实 TCP 覆盖两个不完整源拼接、坏 part 换源和中途断线换源，见 [ed2k-multisource-parts.md](ed2k-multisource-parts.md)。当前为串行多源，持久化/并行调度、Kad、真实互通和 GUI 等仍待完成。

后续进展：已接入可选完整 part 原子缓存及恢复后重新校验，跨进程非析构退出、坏缓存拒绝、离线发布及 TCP 只取缺失 part 已验证，见 [ed2k-persistent-parts.md](ed2k-persistent-parts.md)。任务数据库/GUI 自动恢复、遗留临时文件回收、发布后状态恢复等仍未完成。

后续进展：ed2k 已通过适配器接入 TaskManager 的 SQLite 状态、队列、进度、暂停/继续/取消/重试与缓存位置，见 [ed2k-task-integration.md](ed2k-task-integration.md)。这仍是任务 API，服务器配置界面和 main.cpp 的用户入口尚未开放。
后续进展：Qt ed2k 设置、配置持久化和主窗口添加入口已接通，见 ed2k-qt-entry.md；当前明确标为实验性，不代表真实公网/Kad/回调等已经完成。
后续进展：已实现发布凭据保留到SQLite完成提交，以及发布后跳过析构退出的跨进程恢复；见 ed2k-publication-recovery.md。无凭据或被修改的最终文件不会被认作完成。
后续进展：Kad2 bootstrap 线格式/混合端序、有界真实 UDP 查询及应答来源匹配已实现，见 kad-bootstrap-foundation.md。返回节点仍为未验证候选，路由和文件找源尚未接入，不开放 Kad UI。
后续进展：Kad2 节点查找格式、目标校验、XOR 候选排序和有界两跳真实 UDP 遍历已验证，见 kad-iterative-lookup.md；文件源查询、持久化路由及 Kad 下载入口仍待完成。
后续进展：Kad 按文件hash/大小查询源的单页请求与标签解析已接入真实UDP测试，见 kad-file-source-pages.md。多报文收集、联合找源及下载协调器接入仍待完成。
后续进展：Kad 节点查找到多报文源结果收集已串通，真实 UDP 覆盖乱序/重复、静默、页数预算和身份错误；见 kad-source-collection.md。候选源到文件下载、引导一体化与 Kad 用户入口仍待完成。
后续进展：Kad引导→节点查找→文件找源→TCP分块→内容校验发布的受控连续流程已通过，见 kad-download-pipeline.md；Kad模式尚未接TaskManager/持久设置/UI，公网互操作、节点维护和回调/混淆仍缺。
后续进展：Kad模式已接入TaskManager与Qt找源设置，配置schema2兼容schema1、关闭行为和真实UDP/TCP任务完成已验证，见 kad-task-and-qt-mode.md；长期节点维护/真实公网互通等仍缺。
后续进展：TCP既有连接读写与Kad UDP等待已传递停止信号，30秒静默预算的6种真实socket取消场景在1.5秒内结束，全量29/29通过；见 ed2k-kad-network-cancellation.md。TCP建连和磁盘校验仍非即时取消。
后续进展：磁盘校验、复制、缓存恢复和原子提交前均接入停止检查，确定性IO中断测试确认旧缓存/发布保护，见 ed2k-disk-cancellation.md。
后续进展：TCP建连已使用可取消的原线程Qt事件等待，修复新std::jthread必须在connectToHost前创建dispatcher的实际回归；最小真实TCP和原Kad任务均连续3次通过，最终全量32/32通过。见 ed2k-connect-cancellation.md。
后续进展：真实隔离aMule3.1.0明文直连对端互操作已验证，600000/9740345/9728000字节的原生探针和实际协调器均逐字节及独立SHA256一致，参考进程已正常关闭；见 ed2k-real-amule-interop.md。该参考程序不是产品后端，Kad/公网/混淆等仍未验证。
