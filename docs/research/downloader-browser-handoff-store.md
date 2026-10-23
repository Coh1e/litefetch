# 浏览器接管的持久化准备与原子提交

日期：2026-10-20。本轮实现TaskStore/TaskManager接口，未启用生产IPC监听、注册浏览器host或接入迅雷网络。

## 行为

prepareBrowserHandoff把HTTP(S)任务来源、目标路径、origin/requestId、固定taskId和租约保存到browser_handoffs。此时没有tasks队列记录，任务管理器没有可调度对象，不会开始网络。

commitBrowserHandoff在一个SQLite BEGIN IMMEDIATE事务内完成：校验租约和目标/来源冲突→插入Queued任务→标记handoff为Committed→COMMIT。事务成功后，管理器才把已持久化记录纳入内存队列并调度。若进程在COMMIT之后、内存调度之前退出，下次普通启动会从tasks加载Queued任务。

重复prepare不延长租约、不更换taskId；同一origin/requestId不能换来源或目的地。重复commit返回原提交状态，不再插入记录。用户后来移除已经提交的任务后，重放commit不会复活该任务。

abort只作废Prepared状态，不能撤销Committed任务。过期判断在prepare/query/commit/abort/预留检查时执行；没有生产定时清理器，但任何迟到commit都会先检查到期，不能靠残留Prepared行越过租约。系统时间由未来应用服务提供，浏览器不应指定now。

## 一致性与校验

- 数据库schema升级到4，增加独立handoff表、状态约束及Prepared来源/目标的唯一索引。保留既有任务表字段及状态。旧schema1/2迁移逻辑继续保留；新增schema3→4保留任务测试。
- Committed、Aborted、Expired保留requestId记录；目前没有历史清理或保留期限设置。
- 普通任务添加也会检查预留。TaskStore.save的预留检查和写入在同一写事务中；commit内部复用外层事务，避免部分写入。
- 限定HTTP(S)、格式有效的Chrome扩展origin和requestId、有限租约、已有父目录与不存在的目标；拒绝URL内账号。来源和目标按现有规范化规则核对，保留带签名查询参数。
- 提交时再次检查目标是否已出现；冲突时保留Prepared状态，不覆盖已有文件或假报Committed。
- 目前幂等绑定是实际下载使用的来源和目标；Cookie、Referer、Authorization等回放尚未支持。未来加入这些字段时必须纳入幂等绑定，不能只比较URL。
- 该层仅验证origin格式；实际允许哪些扩展由未来生产IPC服务校验白名单。本轮不把格式检查当作权限认证。

SQLite仍使用WAL/FULL。数据库中的签名URL以明文保留，与现有任务来源字段相同；凭据保护和过期记录脱敏仍待实现，不能声称已经保护敏感任务数据。

## 验证覆盖

download-handoff-store用例覆盖：

- 准备阶段不进入队列、准备重开后不自动运行；重复准备身份与期限不变。
- 来源/目标预留冲突、普通任务保存不能抢占预留。
- 重复提交只有一条队列记录；abort不能回退已提交状态；用户移除后重放不复活。
- 租约边界、过期ID不续租、abort后不能提交。
- 在提交标记更新处用SQLite trigger注入失败，证明先插入的任务也回滚，重试后仅一条记录。
- 真实子进程提交完成后强制终止；重开数据库仍有Committed和一个Queued任务，正常TaskManager重启后只执行一次。
- 两个独立SQLite连接并发准备同一requestId，得到相同taskId。
- 目标在准备后出现时提交拒绝；来源协议和标识格式校验。
- TaskManager准备不执行，commit后进入现有统一调度器，重复commit不二次启动。此用例使用传输替身验证调度，不冒充真实浏览器下载。
- schema3→4迁移保留已有Paused任务。

以上验证包含真实SQLite、真实子进程终止及调度事件；未模拟突然断电或硬件写缓存故障。生产IPC→事务→实际HTTP文件下载的集成测试仍需补齐。

最终完整回归8/8通过，共40.37秒，见logs/downloader-tests-with-handoff-store-recheck.log。此前一次完整运行的BT对话框用例失败未复现：加诊断后单独30次通过、最终完整运行也通过；根因仍未确认，相关原始失败记录保留，不能把后续通过说成已经修复偶发问题。

## 尚未接通

后续更新：BrowserBridge与真实host/HTTP的连接已实现，见downloader-browser-bridge-integration.md。以下段落为本报告当时的状态；浏览器host注册和真实浏览器联测仍未完成。

Native host当前仍只对接测试IPC服务。Qt GUI侧的当前用户IPC监听、扩展白名单、保存目录设置、准备/提交消息处理和安装器尚未实现；浏览器扩展仍默认关闭。不能将这些新接口算作真实浏览器端到端完成。
