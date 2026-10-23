# ed2k 服务器登录与文件找源

2026-10-20。新增 `downloader/src/ed2k_sources.*` 和 `ed2k_server.*`，复用现有 Qt Core/Network，无新增依赖、无 aria2、无 wxWidgets、无独立后台进程。

协议依据为本地 aMule 3.1.0：ServerConnect.cpp 的登录发送、ServerSocket.cpp 的 IDCHANGE/FOUNDSOURCES、DownloadQueue.cpp 的 GETSOURCES 发送，以及 PartFile.cpp 的 AddSources。实现是按线格式编写的独立小模块，不引入 aMule 应用核心。

`ServerConnection` 连接调用方明确指定的服务器，发送基本登录（hash16、历史 ID=0、监听端口、name/version 两个旧式标签），等待 IDCHANGE；保存非零客户端 ID 和服务器功能标志。仅发送已实现的基础格式，不宣称支持压缩/混淆。IDCHANGE 接受 4/8/12/16/20 字节形式，额外字段尚未使用。少量服务器状态、消息和身份通知可以穿插，消息长度校验，身份通知作为不受信任的有界元数据忽略。未知消息严格失败。

找源请求使用 e3/19：hash16+size32；大文件使用 hash16+zero32+size64，必须先收到服务器 LARGEFILES 标志。回复仅接受普通 e3/42：hash16+count8+(clientID32+port16)*count。必须精确匹配身份及总长度，最多 255 项；零 ID/端口丢弃，重复 ID/端口去重。Low ID 保留并标为需要服务器回调，不能当作 IP 直连。High ID 的数值仍是 ed2k 线格式小端值，转换成 QHostAddress 前必须调整 IPv4 字节序。

登录与每次串行查询各自共享总超时（1–60000ms）、最多 64 个包、单包 64KiB 上限；异常关闭连接，禁止未登录查询。没有自动寻找公网服务器、后台重试或搜索资源功能，也没有把找源误作资源搜索。

验证：构建 `logs/downloader-build-ed2k-server.log` 成功；`logs/ed2k-server-tests.log` 的 ed2k 8/8 专项通过（1.68秒）。独立字节向量检查大小文件请求、源列表端序/Low ID/去重；逐个截断及尾随字节拒绝。实际回环 TCP 测试核对完整登录与查询字节，穿插服务器消息，分段返回源列表；错误文件身份和拒绝登录两个负例必须失败。

限制：尚未将找到的源交给下载调度器；没有公网服务器兼容验证、回调/入站监听、加密/压缩、服务器列表和连接保持、IPv6、Kad。昵称编码扩展尚未协商，当前仅验证 ASCII 昵称。GUI ed2k 下载仍未开放；不能宣称完整找源到文件的一站式下载已完成。
