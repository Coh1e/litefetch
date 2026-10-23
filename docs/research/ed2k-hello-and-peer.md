# ed2k 同进程握手与 TCP 连接

2026-10-20。新增ed2k_hello.h/.cpp与ed2k_peer.h/.cpp。使用现有QtCore/QtNetwork，不引入Boost、wxWidgets或独立后台进程，尚未接入GUI调度。

## 格式核对

依据已固定aMule3.1.0本地源码BaseClient.cpp的ProcessHelloPacket/ProcessHelloTypePacket/SendHelloTypePacket、Tag.cpp、ClientTags.h、TagTypes.h及Client2Client/TCP.h核对线格式：Hello有16字节hash的长度前缀，HelloAnswer没有；身份后为小端ID、TCP端口、32位标签数量，再有服务器IP/端口及部分历史扩展尾部。实现独立的Qt边界解析，不链接aMule内部库。

标签识别传统16位名字长度和高位压缩数字ID形式；支持整数宽度、字符串/短字符串、hash、float原始字节、布尔、位数组、blob与短blob。保留未知ID的已知类型值；未知类型因无法可靠确定长度而拒绝。握手限制64KiB、128标签、256字节名字、8192字节字符串；重复关键身份字段和不匹配类型拒绝。历史4/6字节尾部保留，不猜测其含义。

本机Hello仅宣布基础ed2k名称与协议版本，不宣称尚未实现的Kad、压缩传输或eMule扩展能力。不是完整特性协商实现。

## 连接

PeerConnection为工作线程内的阻塞式Qt TCP连接，保留socket供后续数据阶段使用。连接/发送/握手读取共享总期限；接收缓冲有界，支持分片与连续报文；无效帧、握手错误或网络失败不会自动重连。握手目前只支持主动连接后收到HelloAnswer，尚不处理同时发Hello、入站peer、MSE/协议混淆及安全身份认证。Packed帧的负载仍是压缩字节，不会误当已解压内容。

## 验证

- 已知独立字节向量校验编码；Hello/Answer差异、两类标签命名、逐字节截断、超量标签、重复身份、未知类型、非法hash长度及尾部限制检查通过。
- 同一测试进程中启动真实QTcpServer线程，按已知字节检查客户端发出的Hello，分字节返回HelloAnswer，再连续发送两帧；客户端正确识别身份并按顺序接收。
- 超大帧头、关闭前截断回应、静默peer超时均拒绝，测试未启动其他进程。
- 3/3 ed2k专项检查通过，1.26秒：logs/downloader-build-ed2k-peer.log、logs/ed2k-peer-tests.log。依赖guard仍检查44个允许的Boost包；ed2k新模块自身没有Boost依赖。

这是受控peer协议字节和真实TCP验证，不是与外部aMule/eMule互通验证，也不是文件下载完成证明。分块请求/接收、MD4/AICH、服务器源发现、Kad、上传排队、断点恢复和统一模型仍需完成。
