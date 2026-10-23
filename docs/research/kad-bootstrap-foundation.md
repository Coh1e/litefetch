# Kad2 引导 UDP 基础

2026-10-20，新增 `downloader/src/kad_bootstrap.*`、`kad_client.*` 和独立内部构建目标 download-kad。仅链接已有 Qt Core/Network，无新第三方依赖、wxWidgets、aria2 或独立进程。

协议依据为本地 aMule 3.1.0：include/protocol/kad2/Client2Client/UDP.h、Protocols.h、KademliaUDPListener.cpp 的 Bootstrap/Process2BootstrapRequest/Response，以及 SafeFile.cpp 的 ReadUInt128/WriteUInt128、UInt128.cpp 的大端字节转换。新代码按线格式独立实现。

未混淆的 Kad2 引导请求是两字节 e4/01，无 TCP 长度头。回复为 e4/09、节点 ID16、TCP port16、version8、count16、contact[count]，每项 ID16+IP32+UDP port16+TCP port16+version8。ID 的线格式是四个小端 32-bit 字、字序大端；内部统一转为 16 字节大端 ID。Kad IPv4 为 host numeric order，区别于 ed2k 服务器源列表 ID 的 IP 表示。

按参考实现最多返回 20 个 contact，完整数据报上限 523 字节（包括 e4/opcode 两字节），严格检查长度/计数；空/无效节点身份拒绝，候选中的零地址/端口、旧版、重复节点/UDP端点及自引用过滤。Packed e5 尚未实现，不冒充可读的未压缩应答。

`kad::bootstrap` 只向调用方明确指定的 IPv4 seed 发一次请求，创建临时本进程 QUdpSocket。应答必须来自该 IP 和端口，有共享总超时、64 数据报处理上限和有界读取。其他来源不作为回应，超限或目标端格式错误失败。每次查询使用新的本地 socket，避免前次查询的旧数据报留在同一接收队列。此处没有 crypt key/nonce 身份认证；匹配来源的回复仍只提供候选节点，不直接加入可信路由表或作为文件源。

测试 `kad_bootstrap_test.cpp` 使用独立字节向量检查混合字节序、IPv4/端口、空列表、去重、逐字节截断、尾随字节、压缩标记和超额计数；同进程工作线程的真实 QUdpSocket fixture 检查请求恰为 e401，并验证错误端口注入不被接受、目标截断、静默超时及大数据报拒绝。

构建 `logs/downloader-build-kad-bootstrap.log` 成功。专项 `logs/kad-bootstrap-tests.log`：Kad 与 ed2k 14/14 通过（3.76秒）。未在本轮重跑非 ed2k/Kad 测试。

仍未实现：Kad Hello/身份验证、路由桶/XOR距离维护、迭代节点查找、按文件身份找源、候选源到下载协调器、节点持久化、UDP 防火墙与混淆、重试/取消、UI 设置和真实公网互操作。本阶段不开放 UI Kad 开关，不宣称已支持 Kad 下载；既有实验性 ed2k GUI 仍明确显示 Kad 未完成。
