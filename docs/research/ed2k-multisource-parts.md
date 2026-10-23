# ed2k 跨源保留已验证 part

2026-10-20，更新 `downloader/src/ed2k_download.*` 与真实 TCP 协调器测试。没有新增依赖或独立进程。

协调器现持有一个会话级 FileReceiver、已验证 part 位图和经文件 hash 聚合验证过的 hashset。按服务器返回顺序连接 High ID 源，完成文件状态和授槽后，只请求该源拥有且本地尚未验证的 part；每次完整收到一个 part 立即从磁盘验证，成功才记入可信位图。后续源复用已经验证的 hashset，不依赖它再提供可信身份。

请求批次限制在当前 part 内，每批最多三块。损坏 part 的覆盖由 verifyPart 清除；中途断线等失败清除当前未完成 part，先前已通过 hashset 校验的 part 原位保留。下一源不会再请求这些已验证 part，收到缺失 part 后重新校验。所有数据 part 都验证后，仍重新计算整文件 ed2k hash，匹配才无覆盖发布。部分源正常贡献结束会发送 CANCELTRANSFER；失败连接关闭释放槽。

本阶段为串行换源复用，并不是多个 TCP peer 同时并行下载。所有状态仍在当前会话内；进程结束后没有持久化恢复。取消、源耗尽或失败退出会销毁临时文件。该限制不取代后续并行调度、持久化、长队列/Kad/GUI 等完整范围。

真实 TCP 测试保留原先源不可用回退和坏文件不发布，并增加：

- 两个源的位图分别为第一 part 和尾部 part，两者都不持有完整文件，最终必须共同完成文件。
- 第一源正确提供第一 part 后返回损坏尾部，第二源只允许请求尾部；重复请求第一 part 会直接让测试失败。
- 第一源正确提供第一 part 后，在尾部仅发送 1000 字节即断线；换源必须丢弃这个未完成 part 的覆盖，再取完整尾部，且不能重取第一 part。

测试文件大小 9728000+12345 字节，最终逐字节比对，检查请求连续性、part 边界及上传槽释放。预先取消和临时文件清理继续检查。

构建 `logs/downloader-build-ed2k-multisource.log` 成功；专项 `logs/ed2k-multisource-tests.log`：10/10 通过（2.67 秒）。未在本轮重跑非 ed2k 回归，未验证公网 eMule/aMule 互操作。

此文更新此前 ed2k-download-coordinator.md、ed2k-hashset-and-repair.md 中“完整源限定/跨源未接入/每次换源丢弃接收器”的阶段描述；其余未完成项仍有效。4GiB 上限、无 Low ID 回调、扩展兼容不足、无 Kad/恢复/GUI 等限制尚未解除。
