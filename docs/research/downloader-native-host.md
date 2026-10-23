# C++ Native Messaging桥接端

日期：2026-10-20。Windows Qt/C++开发实现，未注册浏览器host或修改注册表；没有执行迅雷组件、使用迅雷DLL或接入迅雷网络。

## 已实现

- native-downloader-host.exe 控制台进程使用二进制stdin/stdout，按Windows本机字节序读取4字节长度和UTF-8 JSON；stdout只写协议。
- 单条消息上限64KiB，拒绝零长度、超长、截断、无效UTF-8、无效JSON和非对象消息。
- 校验version/id/requestId，操作仅prepare/query/commit/abort。prepare仅接受HTTP(S) GET，无URL内账号；额外的请求上下文和策略仍需应用端核查。
- 调用方必须是启动参数中的Chrome扩展origin，并在可执行文件同目录native-host.json的allowedOrigins中明确列出。Chromium传来的可选--parent-window仅接受数字。Firefox启动约定尚未支持。
- 按配置ipcName向QLocalSocket转发消息，把启动参数origin覆盖写入消息，防止JSON自报来源。
- 本地IPC设总等待预算3秒；服务端响应必须匹配消息ID及协议结构。应用离线、错配或异常消息返回关联的ok:false，不把失败当成持久准备成功。
- 不自动启动GUI，不注册host、不分配保存位置、不创建下载任务。配置缺失默认拒绝运行；尚无正式安装器生成配置。

配置格式为 `{ "allowedOrigins": ["chrome-extension://<32位扩展ID>/"], "ipcName": "<本用户应用端点>" }`。本轮只在隔离测试目录中生成实际配置，不向开发包写入占位白名单。

## 构建与真实进程验证

build-local.cmd已构建并部署native-downloader-host.exe。CTest的download-native-host启动真实子进程，在项目内临时目录复制测试程序并写入配置，通过QLocalServer测试服务收发实际帧。服务端使用UserAccessOption。

覆盖：stdin分片、多帧、UTF-8 prepare、伪造JSON origin覆盖、未许可扩展来源、未知操作、file URL拒绝、零/超长/截断长度、畸形JSON、无效UTF-8、IPC错配响应/超长响应、应用离线以及正常EOF。结果见 logs/downloader-tests-native-host.log。

这证明可执行文件、消息边界、来源参数校验与本地IPC转发路径可用。QLocalServer目前是测试服务，不是Qt下载应用的任务事务端；没有真实浏览器调用或实际下载验收。测试服务的ACL不能替代未来生产服务的ACL验证。

本轮完整C++回归7/7通过，共40.15秒，包含受控HTTP/FTP、HTTP续传、BT与GUI选择测试；日志为logs/downloader-tests-with-native-host.log。消除一项测试字节常量转换警告后已重建并单独复跑桥接端测试通过，最终构建日志无编译警告。原生GUI和桥接端均在out/package/bin部署。

## 仍缺的端到端环节

后续更新：应用IPC服务、持久事务和真实HTTP整链已在downloader-browser-bridge-integration.md验证；以下列表保留本报告当时的缺口。真实浏览器调用、安装/注册和用户配置体验仍未验收。

1. 应用侧当前用户IPC服务与权限、浏览器身份配置和显式启用入口。
2. 持久化prepare租约、重复requestId绑定、query/abort，以及commit与SQLite任务记录的原子性。
3. 浏览器host manifest、按用户注册/卸载、稳定扩展ID和Firefox适配。
4. Chrome/Edge真实浏览器下载事件→host→Qt队列→最终文件字节的一致性验证。
5. 应用未运行时的启动与等待策略、保存目录选择及登录上下文回放。

在这些环节完成前，扩展继续默认关闭，弹窗明确提示尚未接通本机下载器。
