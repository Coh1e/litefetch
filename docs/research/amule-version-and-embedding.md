# aMule 维护状态与内嵌可行性核对

2026-10-20，回应用户对后台方案与版本来源的质疑。此文是核对记录，不锁定后台方案。

GitHub实时API结果 analysis/amule-maintenance-check.json：amule-org/amule 未归档，默认master；最新稳定发布3.1.0，发布时间2026-09-21T13:45:06Z；本地使用同一个3.1.0。master头6b0500cb743a361cb8c1b4f3df713524064dfb06，提交时间2026-10-19T17:33:40Z。因此当前没有为兼容而选旧稳定版；固定稳定版而未追master是为了复现实测，不等于master修复可以忽略。

来源：[官方最新发布](https://github.com/amule-org/amule/releases/latest)、[当前仓库](https://github.com/amule-org/amule)、[旧仓库迁移说明](https://github.com/amule-project/amule/discussions/922)。旧 amule-project 仓库的维护状态不能代表当前 amule-org。

内嵌并非技术上不可能。源码src/CMakeLists.txt确有muleappcore/mulesocket等静态库，但amuled仍直接编译CORE_SOURCES、COMMON_SOURCES、amuled.cpp并依赖wxWidgets::NET；muleappcore亦公开依赖wxWidgets::BASE。src/amule.h中的CamuleApp继承应用框架且拥有OnInit等生命周期。这些是内部构建目标，不能因存在STATIC就当作已具备可直接链接Qt的独立协议SDK。

若做内嵌，需要先验证核心启动/停止及单例状态、wx事件/网络循环与Qt共存、配置与持久化入口、线程归属/回调，并封装我们自己的窄C/C++接口。官方Windows发布链使用MSYS2 CLANG64，项目Qt目前MSVC；直接C++链接不能混用两套ABI，必须统一重编译或改为明确的C ABI边界。未实施内嵌构建，不能断言需全面重写，也不能断言只链接现有静态库即可。

已实施后台路径利用现有amuled/amuleapi控制边界，可避免把完整wx应用生命周期放入Qt进程，并能独立更新引擎；代价是额外进程、端口、认证与生命周期管理。它是先前验证路线，不是用户要求的永久架构。用户质疑后，应先比较内嵌可行性与最小维护改动，再决定继续扩展哪个路径。已完成控制/恢复测试可保留作为行为对照。
