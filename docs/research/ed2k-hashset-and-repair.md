# ed2k hashset 与坏 part 重取基础

2026-10-20，新增 `downloader/src/ed2k_hashset.*`；扩展 FileReceiver 和现有传输协调器。无新增依赖、无独立进程，仍仅使用现有 Qt/C++。

参考本地 aMule 3.1.0 的 UploadClient.cpp::SendHashsetPacket、KnownFile.cpp::LoadHashsetFromFile、DownloadClient.cpp::ProcessHashSet，核对 e3/51 请求与 e3/52 回答：文件 hash16 + count16 + part-hash16[count]。

`parseHashset` 严格匹配文件身份、操作码、大小对应的计数和总长度，最多 65535 个 hash。文件 >=9728000 字节时，计数为 floor(size/partSize)+1；计算所有 part hash 字节串的 MD4，必须等于已知文件 hash。恰好整 part 边界时，末尾还必须是 MD4(empty)。较小文件直接以文件 hash 作为 part hash，不向对端请求不存在的 hashset。

`requestHashset` 在连接上发请求并接收有界回答，共享总超时；校验失败关闭连接。现有 downloadFromServer 已接入真实 hashset 请求，并在发布前逐个验证磁盘 part，然后仍保留整文件重新校验。

FileReceiver 新增 `verifyPart` 和 `discardPart`：未收齐 part 不可校验；流式读取该 part 计算 MD4，与调用方提供的已验证 hashset 中对应 hash 比较。不匹配时，仅清除这个 part 的已收覆盖区间和计数，两侧有效区间保留。旧磁盘字节保留但不算已收，新数据可覆盖并重新计数；重新收齐后可再次校验。公开接口要求调用方提供可信 part hash，接收器不自行推断远端 hash 的可信度。

测试 `ed2k_hashset_test.cpp` 覆盖根不匹配、截断、计数错误、整 part 空边界、单小 part，以及两个完整 part 加尾部的落盘：中间 part 被填为错误内容，拒绝后检查两侧内容仍有效，再重填中间 part，最终文件逐字节一致。现有真实 TCP 找源下载测试加入 hashset 请求/回复，坏内容在 part 校验阶段拒绝发布。

构建 `logs/downloader-build-ed2k-hashset.log` 成功。`logs/ed2k-hashset-tests.log`：10/10 ed2k 专项通过（2.53 秒）；本轮没有重跑非 ed2k 测试。

仍需将可信 part 状态用于跨源调度和持久化。当前协调器遇到源失败仍整次丢弃该源的接收器，并非已经完成多源拼接。AICH、Kad、长队列、Low ID 回调、真实互通、恢复和 Qt 任务/GUI 接入仍未完成。MD4 是既有 ed2k 协议身份算法，不代表另加现代安全认证协议。
