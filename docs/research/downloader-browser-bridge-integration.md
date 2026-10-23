# 浏览器本机桥接到真实下载队列

日期：2026-10-20。未安装浏览器扩展、未修改注册表、未使用迅雷DLL或加速网络。

## 已接通

BrowserBridge以QLocalServer接收本机host消息，执行TaskManager的持久准备/查询/作废/提交。GUI新增开发启动选项 `--browser-bridge-config <文件>`；只有明确提供且enabled=true时监听，默认不启用。

配置含enabled、ipcName、allowedOrigins和downloadDirectory。服务端独立核对扩展origin白名单；目录必须绝对、存在且能解析为规范路径，prepare还执行临时文件写入探测。文件名由已解析URL的建议名生成，替换路径分隔符/非法字符、处理普通设备名与尾部点/空格并限制长度。浏览器不能提供任意保存路径；本轮仍按固定目录保存，重名冲突拒绝接管而不覆盖文件。

服务使用UserAccessOption，限制连接数、缓冲大小和连接等待，拒绝多帧混入、超长/非法消息。不会删除已有端点来强行监听。origin文本白名单不是针对恶意同用户进程的密码学身份认证；没有做不同Windows用户账户间的ACL实测。

Windows端点独占还需额外处理：首次完整回归证明第二个QLocalServer可以监听同一管道名。当前先创建并持有按端点名（大小写归一化）散列的QSharedMemory内核对象，再监听；对象已存在时拒绝启动。用独立真实进程及大小写变体核查独占。服务销毁前先关闭监听并断开/销毁客户端，避免回调引用已销毁的成员。失败日志downloader-tests-with-browser-bridge.log与修正后独立测试downloader-tests-browser-bridge-exclusive.log均保留。

## 长连接修正的实际证据

首次download-browser-bridge测试保持Native Messaging stdin打开以继续发送query/commit，45秒超时，日志downloader-tests-browser-bridge.log保留。此前host测试会提前关闭stdin，未覆盖这一交互方式。

QFile/stdio预读会等待大块输入。关闭stdin/stdout的stdio缓冲并给QFile使用Unbuffered后，相同整链用例在0.66秒通过，见downloader-tests-browser-bridge-unbuffered.log。最终用例持续保留“输入流未关闭即必须回复”的条件，防止只在EOF时处理消息。

## 整链测试范围

download-browser-bridge启动真实native-downloader-host.exe，并使用生产BrowserBridge类、真实SQLite、TaskManager默认libcurl传输以及受控本地HTTP服务器。不是伪造commit响应或传输替身。

已验证：

- 实际读取应用配置、监听本机端点；重复监听同一端点失败且原服务仍可用。
- 独立进程使用端点大小写变体也不能抢占现有Windows服务。
- host→服务prepare成功时没有tasks记录、没有HTTP请求。
- 重复prepare保持taskId；commit进入统一队列，256KiB受控HTTP文件完成并逐字节一致。
- 重复commit没有新增任务或第二次HTTP请求；abort不能撤销已提交结果。
- prepare后abort再commit保持Aborted，无HTTP请求。
- host配置允许但应用白名单不允许的另一origin被应用拒绝。
- host正常关闭，没有把URL或消息体写入诊断。

测试在QCoreApplication中运行与GUI相同的服务与管理器实现，没有通过真实浏览器downloads事件，也未把GUI窗口本身作为被测进程。不能将它描述为浏览器自动接管已完成。

## 剩余工作

最终完整C++回归9/9通过，共39.78秒，见logs/downloader-tests-with-browser-bridge-final.log；构建日志无编译警告。原生GUI、host及开发扩展包已更新，未安装/注册浏览器集成。

浏览器host manifest与注册/卸载、稳定扩展ID、GUI配置入口和保存策略、应用未运行时启动、Firefox适配、Cookie/Referer回放、真实Chrome/Edge事件与最终文件联合验收仍缺。扩展开发包继续默认关闭；当前启动选项只是开发接线，不是最终面向用户的配置体验。
