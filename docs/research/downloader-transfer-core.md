# 独立普通传输核心进展

2026-10-18。新版完全使用libcurl8.22.0，不加载/分发旧迅雷引擎。源码downloader/src/transfer.{h,cpp}；Qt Widgets调用在src/main.cpp，独立后台线程传输，进度通过排队事件交还GUI。旧版逆向结果只用于确认需求。

## 已实现与验证

- 仅接受经统一解析层验证的HTTP/HTTPS/FTP；限制初始协议和重定向协议，HTTPS起始不得降级HTTP，禁止跳转file等协议；10次重定向上限。
- 显式目标路径，拒绝覆盖；同目录唯一.part文件，传输成功并flush后rename发布。错误、取消与目标冲突清理本轮临时文件。
- 连接30秒超时、持续低速60秒超时；下载进度、取消；HTTP状态/磁盘/TLS/网络错误的基础分类。未自行关闭证书验证。
- GUI已能输入普通链接、选保存路径、查看进度/速度/结果、取消选中任务。后续已接入SQLite记录和调度，见downloader-task-lifecycle.md。源URL/凭据不放进表格或测试日志，数据库中的源URL尚待凭据保护。
- download-transfer-tests在同进程独立服务线程运行127.0.0.1临时HTTP与FTP，实际传输1MiB确定性二进制并逐字节比较；覆盖Unicode路径、302、404、Content-Length截断、32768字节后取消、已有文件保护、禁止file跳转、.part清理及被动FTP认证。CTest 2/2通过（另一个为链接解析测试）。
- 原始结果：downloader/out/build/windows-local/Testing/Temporary/LastTest.log。受限环境最初拒绝所有本机连接，不能记为通过；获得仅本机网络的运行权限后同一测试通过。
- 打包目录包含libcurl-d.dll；精简PATH真实Windows窗口启动检查通过，截图已查看。界面点击/文件对话框完整自动化尚未验收。

## 明确未完成

这不是最终HTTP调度核心：当前最多3个普通任务、每活动任务一个阻塞worker，已接入持久化队列/优先级和记录恢复；后续HTTP暂停/强ETag条件续传、数据检查点、资源变化核查及发布恢复见downloader-http-resume.md。尚未实现curl multi连接池、分段、用户校验值、代理、浏览器Cookie/Referer及事务接管。HTTP暂停保留恢复数据，取消清理本任务恢复文件。HTTPS已构建Schannel后端，尚缺受控TLS成功/证书拒绝测试。FTP仅验证本地被动模式，暂无续传。BT/ed2k、浏览器扩展、计划任务及完整范围保持未完成。

API依据：[协议限制](https://curl.se/libcurl/c/CURLOPT_PROTOCOLS_STR.html)、[进度与取消](https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html)、[重定向](https://curl.se/libcurl/c/CURLOPT_FOLLOWLOCATION.html)。后续浏览器接管必须等持久化接收与失败回退实现，不能把当前函数返回/GUI添加行当成浏览器可取消的可靠确认。
