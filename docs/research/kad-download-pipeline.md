# Kad 引导到已校验文件的连续流程

2026-10-20，新增 `downloader/src/kad_download.*` 和内部目标 download-kad-download。只复用已有 Qt 与 ed2k 模块，不增加第三方库、wxWidgets、aria2 或独立进程。

ed2k 下载协调器新增 PeerEndpoint（IPv4地址、端口、可选期望userHash）与 sourceProvider 接口；原 downloadFromServer 保留兼容包装，核心由 download 执行。未提供 provider 时沿用服务器登录/查询；提供时以其结果代替服务器找源，不隐式登录服务器。源端点有界（最多300）并去重，原有 part/hashset、多源校验、缓存与发布恢复保持复用。

provider 在完整缓存/发布恢复之后调用，因此已可离线完成时不会先触发网络发现。对带期望身份的源，TCP HelloAnswer 的userHash必须匹配，随后才允许文件确认、排队、分块请求。来源声明和握手匹配不是密码学认证；最终仍依靠已有内容校验，不把来源记录当成文件内容可信证明。

`kad::downloadFile` 安装Kad provider：对最多8个明确seed做有界bootstrap，将回复seed与候选节点交给既有节点/文件源联合查询，默认共享5秒发现预算、最多4个文件源查询节点。筛选directlyUsable源（类型1/4且不要求加密），将规范host-order IPv4、TCP端口和userHash交给协调器。Kad模式没有ed2k服务器登录，不做关键词资源搜索。取消在阶段之间检查，内部UDP阻塞仍受预算限制，尚非即时取消。

真实连续测试 `kad_download_test.cpp` 同进程fixture提供UDP Kad节点和TCP下载peer：检查bootstrap、文件hash节点查找、源查询的原始字节，返回peer源，再检查Hello、文件确认、授槽、多批块请求、CANCELTRANSFER。成功文件600000字节，最终逐字节比对。负例覆盖：

- 源userHash与TCP握手不匹配，连接不能进入文件协商。
- 必须加密的源不发起TCP连接。
- 回调源不发起TCP连接。
- 真实TCP返回损坏内容，不能发布最终文件。

构建 logs/downloader-build-kad-download.log 成功；logs/kad-download-tests.log 为 Kad/ed2k 18/18专项通过（5.95秒），包含原服务器找源、多源part、缓存、任务与设置回归。未在本轮重跑非Kad/ed2k测试。

仍缺：Kad模式到TaskManager/持久配置/UI、路由桶/长期节点维护、Hello/混淆/密钥/回调、即时取消及并行、真实公网互操作等。GUI的Kad入口继续关闭。这里证明的是受控协议对端的完整链路，不是成熟公网Kad客户端。全目标仍未完成，明确排除迅雷加速网络。
