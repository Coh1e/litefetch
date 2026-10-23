# ed2k 文件确认与上传队列

2026-10-20。仅使用现有 Qt Core/Network 和 C++；没有新增库、wxWidgets、aria2 或独立进程。

## 协议依据

核对本地 aMule 3.1.0 源码 `ClientTCPSocket.cpp` 的 OP_REQUESTFILENAME、OP_SETREQFILEID、OP_STARTUPLOADREQ、OP_QUEUERANK、OP_ACCEPTUPLOADREQ 和 OP_QUEUERANKING 处理；`DownloadClient.cpp` 的 ProcessFileInfo/ProcessFileStatus；`PartFile.cpp` 的 WritePartStatus。源码用于核对线格式与行为，新增代码不是将 wx 核心或应用搬入产品。

文件名回复实际是 hash16 + little-endian uint16 字节长度 + 名称，与协议头注释中的 len4 不一致，以实际发送/读取路径为依据。单 part 文件通过文件名回复确认共享；多 part 文件再查询状态。零 part-count 表示完整源；非零计数为 floor(size / 9728000) + 1，包含精确 part 边界的空 part，位图按计数向上取整到字节。数据 part 的可用查询不暴露额外空 part。

## 实现

`downloader/src/ed2k_queue.*` 是独立于 socket 和 UI 的事件状态机：Idle → AwaitingName →（大文件 AwaitingStatus）→ Queued → Ready。无文件或没有可用数据 part 进入 Unavailable；身份、长度、位图计数、非法状态顺序或未知消息进入 Failed。文件名仅保留有界原始字节，不决定本地路径，不假定已协商 UTF-8。

Queued 阶段接受 e3/5c 的 32 位排名和 c5/60 的 12 字节扩展排名，排名不代表上传许可。只有空载荷的 e3/55 允许进入 Ready。部分源可用位图保留给后续调度器选块，并不宣称该源拥有完整文件。

`waitForUploadSlot` 驱动初始文件协商，整段使用共享总期限（最多 60 秒）与 512 包上限，失败关闭连接。长期排队需由任务层直接驱动事件状态机，以支持保活/重连/取消；当前同步辅助函数不承诺无限排队，也没有伪造即时成功。

## 验证与边界

构建 `logs/downloader-build-ed2k-queue.log` 成功，`logs/ed2k-queue-tests.log` 为 7/7 专项通过（1.57 秒）。新增状态测试覆盖单 part、多 part、精确 part 边界、完整源、空位图、两种排名、提前授槽、身份错误、截断名称、错误计数、无文件及终止状态。真实 TCP 测试现已顺序检查 Hello、文件名请求/回复、上传请求、排名、授槽，再传输 500000 字节到最终文件；原错误身份/内容损坏/冲突块负例保留。

仍未证明公网 eMule/aMule 互操作。当前严格拒绝未支持的扩展/压缩/评论消息，尚需完善扩展协商、可忽略消息策略、hashset/AICH、服务器找源、Kad、长时间队列维护、断点恢复、取消及任务模型/Qt GUI 集成。上层调用仍必须遵循先协商授槽、按源位图选块的顺序。全目标尚未完成。
