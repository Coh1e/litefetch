# 新版BT核心：实现与验证记录

2026-10-18。实现downloader/src/torrent_engine.{h,cpp}，依赖固定libtorrent2.1.2，禁用WebTorrent/WebRTC，使用非废弃API（ABI100）。后续已接统一GUI、SQLite任务及文件选择，见downloader-bt-integration.md；本报告保留后端初轮验证记录。

## 接口和实现

TorrentEngine持有一个共享session及独立alert泵；多个worker可调用run，各自保存恢复请求的promise由handle分发，避免不同任务互相取走resume alert。inspectTorrent读取有界本地metainfo，给出名称、完整v1/v2身份、文件列表和大小。

run以新目录为保存单位，只接受本任务归属清单匹配的已有目录。拒绝原始种子中的绝对/父级/Windows保留路径、符号链接和大小写碰撞；恢复参数的renamed_files被清空，保存路径由调用者重新指定。已存在的payload路径逐层检查符号链接/目录联接；这不是对并发本地恶意重解析点修改的完整防护证明。

初始upload_mode和default_dont_download阻止文件数据在元数据校验及文件选择前开始下载。优先级0—7，至少选择一个真实文件；元数据通过后才开启文件请求。恢复数据包含info字典；恢复时调用force_recheck验证实际文件，不直接相信旧have-pieces。检查/优先级变更前的旧status不作为完成依据。

停止时先等待torrent_paused_alert确认全部磁盘IO结束、文件关闭，再要求save_info_dict并保存resume后返回；不能单看save_resume_data_alert当作磁盘完成，因为2.1.2源码在async_release_files未完成时即可发送它。文件优先级也等待file_prio_alert并核对实际优先级后才开放请求。运行中另有30秒保存。部分文件留在任务目录供继续，核心不递归删除用户目录。当前达到选中文件完成后停止，尚未实现做种比例/时间策略。

localOnly测试配置在session启动前禁用DHT/LSD/UPnP/NAT-PMP，绑定127.0.0.1，清除Tracker、web seed、DHT节点及恢复的peer列表，禁用PEX，并以IP filter限制回环；仅接受显式回环peer。会话全局限额也应用本地peer，避免本地默认豁免。

## 已执行的验证

download-torrent测试生成两个各2MiB的确定性二进制文件及v1、v2、hybrid种子，启动仅回环做种端；三种模式真实传输后逐字节比较两个文件，磁力模式确认metadata获取。另执行：

- v1磁力部分下载在1,294,336字节时停止，保存resume，新session完成至4,194,304字节并逐字节比对；该字节数是首轮记录，后续调度可变化。
- 外部修改已完成piece，恢复时没有提供来源且清空恢复peer列表，必须重校验并保持未完成；重新提供本机来源后修复，实际文件重新与原始数据一致。
- 单文件优先级7、另一文件0，实际只产生所选2MiB文件；另一文件未创建。
- 同一共享session同时执行两个不同torrent，保存恢复alert正确分发，两个任务及内容检查通过。
- 已存在的非本任务目录保留原文件、不合并；非法bencode拒绝；a、a-b、a/c这类排序不相邻的文件/祖先目录碰撞拒绝。

首轮BT专测25.80秒通过；补充路径碰撞检查后，全套CTest **5/5通过**，BT专测25.45秒，总33.32秒。原始结果在logs/downloader-tests-bt.log和downloader/out/build/windows-local/Testing/Temporary/LastTest.log。首次长时间依赖安装前CMake已读取旧AST，新目标未进入生成结果；完成依赖后显式重新配置，确认测试exe存在，再执行以上真实测试。test-local.cmd现启用--no-tests=error，空选择不能返回假通过。

固定依赖：libtorrent2.1.2、Boost1.92.0、OpenSSL3.6.5；均在downloader/.deps，未全局安装。Debug/Release依赖构建约14分钟；后端及测试为本机Debug。LibtorrentRasterbar导出ABI100，适配层使用layout()、add_torrent_params版make_magnet_uri，不启用废弃API来掩盖版本差异。

后续已实现统一任务接收/SQLite持久化、原种子缓存与规范化、磁力选择界面及v2选择性暂停恢复。做种控制、代理/全局限速设置、公网DHT/Tracker/uTP、hybrid故障恢复和更全面界面交互仍未完成，不能因回环验证通过宣称完整BT产品已交付。v1损坏修复和v2缓存恢复均有对应场景，具体见接入报告。

API依据：项目本地vcpkg源码libtorrent2.1.2/include/libtorrent及[官方2.1会话](https://libtorrent.org/reference-Session.html)、[状态/恢复接口](https://libtorrent.org/reference-Torrent_Handle.html)、[alert](https://libtorrent.org/reference-Alerts.html)。tutorial页面标为1.2.9，不能将其旧API直接套到ABI100，实际以固定版本头文件为准。
