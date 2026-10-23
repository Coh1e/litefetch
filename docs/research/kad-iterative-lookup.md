# Kad2 有界节点查找

2026-10-20，新增 `downloader/src/kad_lookup.*`，扩展 kad_client，复用已有 Qt Core/Network；没有增加第三方依赖、wxWidgets、aria2 或下载后端进程。

格式核对本地 aMule 3.1.0 KademliaUDPListener.cpp::ProcessKademlia2Request/Response 与 Search.cpp。请求 e4/21 + count8 + targetID16 + receiverID16，ID 沿用四个小端32位字的混合字节序。请求 count 只允许 1–31（对端读取 type & 0x1f，32 会变零），当前询问20项。应答 e4/29 + targetID16 + count8 + contact25[count]，必须匹配当前 target、长度及请求数量。

`xorDistance` 使用规范大端 ID 计算16字节 XOR。`lookup` 复用有界 UDP 交换器，验证来源 IP/端口、大小及共享超时；原有 bootstrap 已改为复用同一交换器，并重新回归。应答节点列表去重、过滤无效端点，尚无加密身份认证。

`findClosest` 接受明确的候选 seeds，在单工作线程按 XOR 距离依次查询，插入新候选并继续。默认最多32次查询、256候选、10秒总预算，单查询默认1秒；调用参数最多64次/60秒，单次最多处理64个UDP报文。节点ID和端点均记录已尝试，避免循环/别名重复查询；候选满时仅保留更近节点。失败、超时或格式错误不标为 responsive，继续尝试其他候选。最终最多返回20个实际应答节点并排序，同时报告预算是否耗尽。

该实现是会话内 shortlist 遍历，并非已完成持久化 Kademlia 路由桶、并行 alpha 查询或长期节点维护；responsive 仅表示来源匹配且目标匹配的 UDP 应答，不等同密码学验证。

测试 kad_lookup_test.cpp：独立已知字节向量、ID混合端序、XOR、逐位置截断、目标错误和尾随数据。真实 UDP fixture 有两个节点，首节点返回更近节点与指回自身的记录；客户端必须查询第二节点，完成后最近节点排序正确且不会循环。另测查询预算1时终止，以及目标不匹配不能标记有效。

构建 logs/downloader-build-kad-lookup.log 成功，logs/kad-lookup-tests.log 的 Kad/ed2k 15/15 专项通过（4.35秒），含 bootstrap 重构回归。未在本轮重跑非 Kad/ed2k 测试。

仍需：Kad Hello/身份与防火墙/混淆、路由桶及节点持久化、按文件hash找源并交给下载协调器、取消/并行/更多故障、真实公网互通。Kad UI 继续关闭，完整目标尚未实现；不包含关键词资源搜索或迅雷加速网络。
