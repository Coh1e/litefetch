# ed2k 发布文件与数据库提交之间的恢复

2026-10-20。补充 PartCache 的发布凭据、下载协调器恢复路径与 TaskManager 完成确认，不新增依赖或后台进程。

发布前，缓存以 QSaveFile 原子提交 .publishing 凭据（版本、绝对目标路径、文件 hash 和大小）。任务适配器设置 deferPublicationAcknowledgement=true：文件发布后清除体积较大的 part/hashset 缓存，但保留凭据，直到 TaskManager 将 Completed 成功提交 SQLite 后清除。数据库提交失败或进程在两步之间退出时，凭据仍可用于恢复。直接调用协调器的其他调用方默认在返回成功时清理全部缓存。

恢复发现最终路径已存在时，要求当前任务缓存中的凭据严格匹配目标路径、大小和 ed2k 身份，并重新读取整个文件计算 ed2k hash。全部匹配才返回完成，期间不重新下载也不改动该文件。没有凭据、凭据无效、文件被修改或读取失败均不能据此接受完成，既有文件不覆盖。

TaskManager 的 ed2k 取消恢复、从头重试、移除和完成清理不再复用 HTTP 临时文件检查。首次专项测试暴露：恢复记录使用非 HTTP 格式任务 ID 时，HTTP 检查会拒绝移除记录，测试失败；修正后仅通过 ed2k 自己的 hash+size/任务缓存路径清理。最终文件从未因该失败被删除。

测试扩展 ed2k_task_test.cpp：

- 文件已发布、数据库仍 Running 的确定性快照，重载为 Interrupted，继续后校验并提交 Completed，凭据随后清理。
- 发布文件被篡改，即使存在凭据也不得恢复完成；文件字节保持不动。
- 文件内容恰好正确但没有凭据，仍拒绝把既有同名文件认作本任务完成。
- 一个独立测试进程真实写入 SQLite Running、原子提交凭据、实际发布文件、保留凭据，然后 std::_Exit 跳过析构退出。新进程回收遗留锁并通过 TaskManager 完成恢复，另开 TaskStore 核实 Completed 已落库。

测试子进程仅用于故障注入，不是下载产品后端。构建 logs/downloader-build-ed2k-publication.log 成功，专项 logs/ed2k-publication-tests.log：13/13 通过（4.09秒）。此前自动审批服务额度曾导致一次构建未执行；同一授权构建重试后成功，没有绕过审批。

边界：覆盖提交后跳过析构的实际进程退出及确定性故障快照，没有验证断电、任意存储故障或原子提交中途硬杀。遗留随机临时文件回收、取消延迟、并行调度、Kad/Low ID、扩展/加密和真实客户端互通等仍需完成。用户界面保持实验性。
全量回归 logs/downloader-tests-ed2k-publication.log：23/23 通过（44.30秒），覆盖 HTTP、BT、任务存储、Qt设置和浏览器接管桥接。
