# HTTP暂停、字节续传和发布恢复

2026-10-18。实现downloader/src/http_resume.{h,cpp}，TaskManager普通HTTP路径默认调用；FTP保留原传输路径。Qt界面提供暂停、继续和从头重试，按状态及协议启用操作。数据库v1→v2事务迁移保留已有任务，新增Pausing/Paused；迁移用旧schema记录实测。

## 实现

恢复文件为目标名.UUID.part及.json，归属于已持久化UUID任务。清单绑定原始URL、最终有效URL、目标路径、强ETag、总长、已提交字节和前缀SHA-256。每4MiB或1秒刷新检查点，正常停止再提交最后前缀；重启仅信任已提交长度，验证前缀哈希，丢弃其后的未提交尾部。

强ETag存在且前缀核对通过才发送Range与If-Range。206必须匹配起点、终点、总长、ETag及最终有效URL；有效URL限定防止不同资源碰巧使用相同ETag时拼接。200说明资源变更或忽略Range，先截断旧文件再写新内容。弱/缺失ETag、恢复元数据损坏、前缀校验失败时从头接收，不混合新旧字节。尚未使用Last-Modified作为续传条件。

拒绝冲突的长度/范围/标识头、非identity内容编码、不正确的206以及不支持的重定向协议；响应尾部字段不重置文件。零字节文件可正常发布。只有强ETag/有效URL/总长相符且完整前缀已验证时，416可用于确认已收完的文件，不能把任意416当成功。

完成后先写downloaded/ready清单，再rename发布，数据库成功提交Completed后移除ready。若rename后进程退出，继续时重新核对目标大小与SHA-256，匹配才认定已发布；已有目标内容不符则拒绝覆盖。下载完成但尚未rename的downloaded/ready检查点可本地验证发布。此处只处理已验证的进程崩溃情形，不宣称文件系统与SQLite组成了跨文件原子事务或已验证断电一致性。

暂停请求先落库，再通知worker停止，停止后保存Paused；继续保留检查点。从头重试丢弃该任务检查点。取消会清理该任务的part和清单，不删除已发布目标。程序正常退出保存检查点，运行记录重开后仍标Interrupted，等待显式继续；Pausing重开为Paused。

## 测试范围

download-resume使用本机受控HTTP服务，真实发送二进制：

- 正常206续传、Range忽略的200回退、强ETag变更、弱/缺失ETag从头接收、chunked尾部。
- 错起点/不一致ETag的206拒绝且旧前缀不改；重定向目标改变但ETag相同仍拒绝。
- 本地片段被修改时重下；检查点后有额外尾部时截回可信长度。
- 零字节、已满前缀的416、downloaded阶段、rename后ready阶段及目标文件被外部修改。
- TaskManager真实暂停、关闭、重开、继续并核对完整内容；暂停后取消清理。
- 子进程真实下载8MiB资源，确认非空检查点已提交后强制终止，父进程按已提交偏移Range继续并逐字节比较完整内容。这不是仅构造临时文件模拟续传。

测试开发中首次45秒超时来自4KiB分块/每块休眠在Windows定时粒度下累计过长；改为64KiB分块后同断言约6秒完成，未放宽超时。另查Qt6.11.2源码确认waitForNewConnection不检查已有pending队列，修正测试服务等待条件；双任务真实传输15次回归通过。调试输出已移除。

## 尚缺

HTTPS受控TLS/证书验证、认证上下文、代理、分段/连接池、公平调度/限速、FTP续传、Last-Modified策略、用户提供SHA-256、磁盘/文件系统更多边界、全应用全部崩溃时间点及断电验证仍未完成。源URL和恢复清单中的敏感信息保护未完成；源URL不显示在列表不等于加密。恢复数据错误不能被包装为浏览器接管成功。

依据：[Range限制及服务器可忽略Range](https://curl.se/libcurl/c/CURLOPT_RANGE.html)、[HTTP If-Range语义](https://www.rfc-editor.org/rfc/rfc9110.html#name-if-range)、[响应头回调](https://curl.se/libcurl/c/CURLOPT_HEADERFUNCTION.html)、[最终有效URL](https://curl.se/libcurl/c/CURLINFO_EFFECTIVE_URL.html)。
