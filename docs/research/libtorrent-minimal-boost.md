# libtorrent 的 Boost 依赖裁剪

2026-10-20。用户确认保留libtorrent，禁止全量Boost，只允许最小必要模块和传递闭包；不采用aria2。

现有libtorrent 2.1.2 Debug构建的Ninja依赖记录导出到logs/libtorrent-header-dependencies.log，按vcpkg文件归属映射：1565个Boost头文件，31个实际观察模块，见analysis/libtorrent-boost-observed.json。该观察只覆盖当前Windows配置，不能推导所有平台/未来功能都不需要其他模块。

独立目录work/libtorrent-minimal使用原版本、原源码摘要和原补丁，仅调整配方Boost依赖：去掉未观察到的旧模块根，Asio明确关闭默认deadline-timer/spawn。首次37包候选缺少Boost.Range头文件，编译失败，保留logs/libtorrent-minimal-build-first-failure.log；按观察模块补齐后得到44个Boost前缀包的有效闭包。

独立Debug与Release均构建成功，logs/libtorrent-minimal-build.log。以精简版构建现有任务/BT/原生文件选择测试，3/3通过，实际本地v1/v2/hybrid/磁力与恢复用例沿用原测试，logs/libtorrent-minimal-tests.log。运行中核对模块确为work/libtorrent-minimal/installed/x64-windows/debug/bin/torrent-rasterbar.dll，证据analysis/libtorrent-minimal-loaded-module.json；关闭vcpkg自动复制DLL，避免旧发布库遮蔽测试。

原Release DLL为4920320字节，精简后4920832字节，基本不变。改进是减少安装/构建依赖，不是已证明运行内存、性能或安装包下降。已有包原本没有分发独立Boost DLL，不能将删包数量算作等量运行库节省。

已将验证配方复制到downloader/overlays/libtorrent，manifest明确boost-asio default-features=false，vcpkg-configuration启用项目overlay。downloader/boost-allowlist.json记录44模块版本闭包；构建入口增加状态检查，禁止wxWidgets、Boost总包、未审查模块/版本和Asio两个默认功能。产品目录重新配置与最终回归需以logs/downloader-minimal-boost-configure.log及后续最终测试日志确认，不能用独立测试代替产品验收。

当前44是已验证缩减闭包，不宣称数学上的绝对最小。若继续裁剪配方保守传递依赖，必须保持独立构建、ABI配置和实际协议/恢复回归。

产品接入结果：正式installed现为61包，其中44个Boost子包（此前80/63），没有wxWidgets。产品构建与部署成功，11/11测试通过，logs/downloader-tests-minimal-boost-final.log（41.15秒）。新增依赖检查在实际构建入口通过；最终显式重配确认DOWNLOADER_BUILD_AMULE_EXPERIMENTS=OFF且Ninja不再生成aMule后台实验目标，logs/downloader-final-configure-no-amule.log和downloader-build-minimal-boost-final.log。初次长时间配置期间修改CMake后，生成器曾保留旧目标；最终显式重配已纠正，不能只看源文件默认值来声称目标已关闭。
