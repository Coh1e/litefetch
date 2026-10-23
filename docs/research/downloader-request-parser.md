# 新版统一任务入口：解析层

2026-10-18。源码位于downloader/src/download_request.{h,cpp}，独立download-domain静态库；后续已接入Qt Widgets主窗口与TaskManager的普通下载入口。解析模块自身不执行下载，不写文件，不选择保存路径。

目前接收HTTP/HTTPS/FTP URL、BT v1/v2/hybrid磁力链接、ed2k文件链接、本地已有.torrent路径或file URL。普通URL移除片段但保留查询顺序和编码；磁力v1的Base32/hex归一成同一身份，hybrid保留两个身份供后续去重。ed2k用无符号64位大小和MD4值建立身份。建议文件名仅为提示，不可直接用作保存路径；路径清理和冲突策略必须由目的地模块负责。

本地种子当前只验证路径/后缀/文件存在，不验证bencode或info hash。ed2k可选段保留在source，但其语义尚未解析。未实现批量输入、thunder包装解码、下载核心或浏览器任务上下文。这些不能因基础解析通过而记为完成。

构建：downloader/configure-local.cmd、build-local.cmd。检查：downloader/test-local.cmd。CTest download-request于本轮通过，原始结果见downloader/out/build/windows-local/Testing/Temporary/LastTest.log。覆盖HTTPS编码与片段、FTP、无效端口/协议/换行、磁力Base32/hex等价与hybrid、无效哈希、ed2k Unicode/溢出、种子路径/缺失文件。只证明解析层。

本轮首次编译遇到MSVC C1902（Program database manager mismatch），改用CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded后编译/链接/打包通过。调试信息写入项目对象文件，避免编译期共享PDB服务；不改变父进程环境或全局配置。
