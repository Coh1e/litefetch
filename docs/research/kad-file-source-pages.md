# Kad2 按文件 hash 查询源结果页

2026-10-20，新增 `downloader/src/kad_sources.*`，扩展 kad_client::querySourcesPage；仅使用现有 Qt Core/Network，没有新依赖、wxWidgets、aria2 或后台进程。

格式核对本地 aMule 3.1.0 的 KademliaUDPListener.cpp::Process2SearchSourceRequest/Process2SearchResponse、Indexed.cpp::SendValidSourceResult、Search.cpp::ProcessResultFile、DownloadQueue.cpp::KademliaSearchFile 和 SafeFile.cpp::ReadTag/ReadTagPtrList。

请求 e4/34 + fileID16 + start16 + fileSize64，start 只允许0..32767，ID使用Kad混合字节序，文件大小为小端64位。应答 e4/3b + responderID16 + fileID16 + count16，后接 sourceID16 + tagCount8 + tags。解析必须匹配本次文件 hash 和指定响应节点 ID，实际 UDP 同时要求来源IP/端口匹配。该匹配不是密码学身份认证。

参考实现一次数据报最多50个源，总结果可拆成多个报文。新API明确只返回一个结果页（最多50项、60KB、每项最多64标签、名字128字节、字符串4096字节），不宣称已收齐多报文流。结果记下原始条目数与过滤后的候选源。

Kad 标签按类型8位+名字长度16位+名字+类型对应值读取，支持参考读取器的 hash16、字符串、uint8/16/32/64、float原始值和BSOB。未知类型严格拒绝，已知类型的未知字段有界跳过。识别 sourcetype/IP/TCP/UDP/encryption/可选 filesize，检查整数范围和重复关键字段，不截断大整数。文件大小不匹配、无地址/端口、无效或重复源过滤。

源类型1/4可直连，3/5/6需要回调，类型2及未知类型忽略。要求加密的源不作为当前 directlyUsable；保留标志给后续协调器决定。源userHash转换为规范16字节身份，Kad IPv4仍是host numeric order。解析不使用远端文件名或创建本地下载任务。

测试 kad_sources_test.cpp：独立请求/应答字节向量、混合ID端序、源IPv4/端口、每个截断位置、尾随字节、文件和响应节点错误、回调/强制加密、重复关键字段、端口溢出和文件大小不符。实际QUdpSocket fixture核对请求字节，正常页通过，错误node/file分别失败。

构建 logs/downloader-build-kad-sources.log 成功；logs/kad-sources-tests.log：Kad/ed2k 16/16专项通过（4.60秒）。未在本轮重跑非Kad/ed2k测试。

尚未完成：多报文结果收集/分页、迭代查找与源查询联合流程、候选源接入下载协调器、Hello/密钥与节点维护、路由持久化、回调、取消及真实公网互通。界面继续提示Kad未完成，没有开放Kad开关。没有增加关键词资源搜索或迅雷加速网络。
