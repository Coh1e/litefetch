# BT 引擎与最小 Boost 约束

2026-10-20。用户明确禁止全量Boost，仅允许最小引入；现有63个boost前缀安装包不构成已完成最小化的证明。当前manifest只直接声明libtorrent且default-features=false，没有声明Boost总包；但vcpkg libtorrent配方列出14个Boost模块，asio又默认启用deadline-timer/spawn，后者引入context。这是优先审核的裁剪位置，不能只凭未出现spawn include就删除依赖；需要实际头文件闭包、独立重建和现有v1/v2/hybrid/磁力恢复测试。

候选：

- libtransmission：当前稳定4.1.3。官方CMake有INSTALL_LIB及关闭GUI/daemon/test的选项，库链接清单不含Boost，但含libevent、curl、crypto、deflate、DHT、UTP、NATPMP、miniupnpc、fmt、small、fast_float、PSL及其他工具库。适合做无Boost候选，不等于体积/内存已实测更小。4.1.3 torrent-metainfo.cc仍将v2文件树/哈希层标为TODO，不能替换后默默丢失现有纯v2能力。来源：https://github.com/transmission/transmission/releases/tag/4.1.3 、https://github.com/transmission/transmission/blob/4.1.3/CMakeLists.txt 、https://github.com/transmission/transmission/blob/4.1.3/libtransmission/CMakeLists.txt 、https://github.com/transmission/transmission/blob/4.1.3/libtransmission/torrent-metainfo.cc 。
- libaria2：官方支持构建为C++库，可覆盖普通下载和BT。Windows可用系统Schannel，README未列Boost依赖；实际依赖/构建及v2功能仍需核实。它会与当前curl调度/恢复实现重叠，不能因项目自称lightweight就认定替换成本低。来源：https://github.com/aria2/aria2/blob/master/README.rst 。
- rakshasa/libtorrent：与当前rasterbar不是同一项目，官方定位Unix，Windows优先项目不作为首选。来源：https://github.com/rakshasa/libtorrent 。
- anacrolix/torrent：官方声明支持v2等协议并以Go库使用为主；Qt/C++项目需额外语言运行时/接口边界，没有本机体积或内存数据，不据此推荐为轻量替代。来源：https://github.com/anacrolix/torrent 。

暂不更换引擎、不安装新依赖。比较时需分别量化依赖闭包、Release发布体积、空闲/活动内存、功能覆盖和维护成本；不能以包数量或源码行数代替这些指标。已有无wx重建与现有10项回归通过，日志downloader-build-after-wx-removal.log和downloader-tests-after-wx-removal.log。
