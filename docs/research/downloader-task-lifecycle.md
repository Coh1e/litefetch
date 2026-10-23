# 新版持久化任务与调度

2026-10-18。实现为downloader/src/task_store.{h,cpp}、task_manager.{h,cpp}，使用SQLite3.53.4。与闭源迅雷二进制无链接关系。

## 行为与边界

任务经过统一解析后，先提交Queued记录，再返回新增结果，之后调度；尚未接入的BT/ed2k拒绝接收，不返回虚假成功。运行前再提交Running状态，若存储失败则取消活动传输、阻止新任务和调度，并通知界面。SQL使用绑定参数；WAL、FULL同步、内存临时表，数据库/日志保存在指定便携数据目录。GUI用QLockFile避免两个窗口调度同一数据库。

当前状态为Queued、Running、Cancelling、Completed、Cancelled、Failed、Interrupted，以及Pausing、Paused。schema v3另保存BT元数据BLOB/文件优先级；v1/v2迁移到v3。普通下载与BT最多共用3个活动名额。按优先级选择，同优先级保持递增序号；活动任务停止需等待worker。HTTP从头重试/清理临时文件，BT继续和重试保留数据并校验，取消/移除记录也保留BT数据。打开文件/任务目录走系统关联；BT完整哈希别名和Windows目的地均用于重复检查。

重开时Running转Interrupted、Cancelling转Cancelled、Pausing转Paused；Interrupted不会自动重放请求。Queued继续调度。后续HTTP字节检查点/强ETag条件续传已接入，见downloader-http-resume.md；FTP仍从头下载。进度只有终态结果落库，实时数字不被当作已提交字节证明，字节恢复以独立清单和前缀哈希为准。应用退出会通知并等待worker停止。

## 证据

- CTest download-tasks：并发上限2、最高优先级任务先派发、队列取消不派发、活动取消释放名额、重复URL/目的地、重试、Unicode/引号、关闭重开、记录移除。
- 测试复查发现同毫秒记录在重开后按随机ID排序的缺陷，修复后补精确FIFO序列比较；针对该时序问题连续运行download-tasks 20次通过。
- 存储故障：用SQLite触发器拒绝Running提交，验证报告错误、runner未被调用、后续新增被拒绝，不只测试正常SQL返回。
- 崩溃：独立子进程提交并进入Running后输出确认，父进程强制终止，再开数据库识别Interrupted，不自动派发。它证明记录恢复，不证明下载数据续传。
- download-transfer：真实本机HTTP内容/重定向经过TaskManager，完成后关闭重开，比较终态和文件内容；独立FTP真实传输检查保留。
- 部署版加载测试数据库，smoke报告task_rows=1，截图可见“上次运行中断”，未发起测试URL网络请求。使用download-task-tests --seed-ui构造的明确合成UI记录，不声称原版运行或实际断点续传。
- 原始日志：downloader/out/build/windows-local/Testing/Temporary/LastTest.log；GUI报告/截图：downloader/out/verification/qt-smoke-package.json及.png。

## 仍需完成

HTTP字节续传、UUID恢复文件归属、前缀资源验证和ready发布清单已有实现与进程终止测试，不能外推到FTP/BT/ed2k或断电场景。全局限速/站点限额/连接池、跨协议适配、凭据保护、设置和完整交互验收均未完成。源URL目前保存在便携数据库及恢复清单，可能含登录或签名信息；未写到表格/测试日志不等于已加密。不能据此直接启用浏览器自动取消：接收Queued的可靠确认已有基础，浏览器租约、提交/回退、下载接管与各浏览器实测尚未实现。

依据：[SQLite事务](https://www.sqlite.org/transactional.html)、[参数绑定](https://www.sqlite.org/c3ref/bind_blob.html)。SQLite事务保证仅覆盖数据库事务，不覆盖独立文件rename或远端请求。
