# ed2k 请求批次的真实 TCP 到文件验证

2026-10-20，继续遵循 Qt 唯一 UI、libtorrent 保留且裁剪 Boost、无 aria2、无 wxWidgets、无独立 ed2k 后端进程的约束。

新增 `downloader/src/ed2k_transfer.*`，链接现有 download-ed2k-peer 和 Qt，没有新依赖。`receiveRequestedBlocks` 面向已完成握手且已获上传槽的连接：发送一批 1–3 个请求范围，接收未压缩分块，验证身份及请求范围，写入 FileReceiver，并维护本批尚未收到的区间。收到整批后返回实际新增字节数，由调用方决定后续批次和最终 publish。

同一批范围内允许相同数据重传，冲突交给接收器拒绝。整批共享最多 60 秒的总超时，不因重复数据延长；最多 4096 个包和 4096 个未收片段。64 位回复必须由调用方明确传入已协商支持。网络、格式或落盘错误会关闭连接。此接口不自行推断上传槽、不自动重试、不隐式发布。

`downloader/tests/ed2k_transfer_test.cpp` 在同一进程内使用工作线程的 QTcpServer 和真实回环 TCP：完成 Hello/HelloAnswer，独立检查请求帧的 starts[3]/ends[3] 字节布局，返回 500000 字节文件的三个范围，乱序拆分并插入重复块；最终文件逐字节比对，新增进度保持 500000。错误文件 hash、内容损坏和冲突重复块三个负例都不得发布最终文件。对端发送接收使用操作系统 socket，并非直接调用数据块解析函数代替传输。

构建：`logs/downloader-build-ed2k-transfer.log` 成功。专项回归：`logs/ed2k-transfer-tests.log`，6/6 通过（1.59 秒）。未在本轮重跑其余下载器测试。

限制：受控对端不等于真实 eMule/aMule 互操作；尚未完成文件状态/文件名/上传排队协议、服务器找源、Kad、压缩、断点恢复、取消和任务模型/GUI 集成。目前不对用户开放 ed2k 链接下载。仍需按完整范围继续实现，不把局部端到端测试视为完整引擎完成。
