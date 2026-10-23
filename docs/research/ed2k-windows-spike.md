# ed2k/Kad Windows 后台验证

日期：2026-10-18。结论：官方 Windows 后台及控制接口的本机验证通过，适合作为下一阶段候选；并非全部协议验收完成。

## 固定输入

- 上游：https://github.com/amule-org/amule ，版本 3.1.0。
- 源码解包记录的 commit：909d304d993ee07df6c6f6acf501a6d791d53666。
- Windows 官方 ZIP SHA-256：81d3a024207c272524e4d992bfaf352a86256b00afac414f0405ce465642cfb7。
- 官方源码包 SHA-256：a05f9b655f4407f18179640f65ae50c93bd36a90c268c8fff7adc575fe8eb041。
- 两个摘要均与 GitHub release API 的 digest 相符；原始清单保存在 work/downloader-spike/release.json。
- 便携工具：tools/amule-3.1.0/amule-portable-x64/bin。
- 源码：work/downloader-spike/source/amule-org-amule-6ccbfdf。

## 实测

使用 scripts/probe-amule.ps1，先执行 -Mode run -Stage create，再单独执行 -Mode run -Stage restore。
需要本机回环网络权限。受限执行环境第一次无法连接回环端口，获准在受限环境外运行后通过。

所有显式配置、日志、下载/临时目录均位于项目。只设置测试子进程的隔离路径，不修改父进程或系统环境变量，不注册 URL 协议、自启动或安装服务。
关闭公网 P2P 自动连接、ed2k/Kad、UPnP、版本检查与 GeoIP 下载。本次不改变防火墙规则。
测试使用一个 32 KiB 合成任务，没有真实来源和已下载数据。

通过的 13 项检查：

- 创建阶段 6 项：添加被接受、任务可读取、暂停、继续、上传限速回读、下载限速回读。
- 重启阶段 7 项：核心恢复非活动状态、应用侧保存暂停意图、统一状态映射、优先级恢复、上传限速恢复、下载限速恢复、删除测试任务。
- 另已验证 API 认证及 version/status/downloads/preferences 读取。
- 最终后台通过 amulecmd shutdown 正常退出，API 进程被测试脚本收回。
- 机器可直接运行官方包中的 amuled、amuleapi、amulecmd，无需安装 aMule GUI。

证据：logs/downloader-spike/checks-create.json、checks-restore.json、task-restored.json、health.json 以及后台退出日志。

## 发现的适配要求

初次恢复检查两次失败：重启前 paused，重启后 stopped。
已排查正常关闭和优先级持久化；源码 src/PartFile.cpp 的 FT_STATUS 加载分支设置 m_stopped=m_paused，保存时只写 m_paused 位。
因此这不是单靠等待保存可以修复的状态差异。

应用必须把用户操作意图与引擎原始状态分别存储。恢复时，如用户意图为暂停且引擎为停止，界面可显示暂停并保留原始状态供详情查看；显式继续时再下发 resume。
本次只验证了该映射方案，不声称新下载器的正式适配层已经实现。

Windows 路径写入 wx 配置时必须处理反斜杠转义；验证脚本使用正斜杠，确认路径回读正确。
引擎中途任务删除会删除 part/met 并记录 canceledfiles；统一界面的“删任务但保留临时数据”和“重新添加”需要单独设计，不能直接套用删除接口。

## 构建和许可核对状态

- 官方 3.1.0 CI 使用 MSYS2 CLANG64；可构建 BUILD_DAEMON、BUILD_AMULEAPI、BUILD_AMULECMD。
- 当前机器有 MSVC、Windows SDK、Visual Studio 附带 CMake/Ninja；未发现现成 Qt/MSYS2 开发依赖。
- 尚未执行本地 aMule 源码构建，官方二进制运行通过不能替代源码可重复构建验收。
- 源码 LICENSE.md 声明 GPL-2.0-or-later；正式项目许可证和所有分发依赖的许可证仍需统一核对。本次不擅自锁定项目许可证。

## 下一阶段

2026-10-20：已新增并实测原生 C++ aMule API 客户端，认证、任务控制与快照均通过真实离线后台，原13项检查重跑通过。详见 [downloader-ed2k-native-client.md](downloader-ed2k-native-client.md)。这只完成下面第2项的一部分，进程生命周期、统一模型接入与真实传输仍缺。

1. 在项目内准备固定版本的 Qt、libcurl、libtorrent 开发依赖，建立原生 GUI 和统一任务模型。
2. 把已验证的 ed2k 控制流程实现为独立适配层；正式运行使用随机本机凭据、端口和受控进程生命周期。
3. 完成合法受控文件的实际 ed2k 数据传输、部分数据重启恢复及完整性校验。
4. 验证服务器/Kad 连接、端口诊断与代理覆盖；当前离线合成任务测试不覆盖这些行为。
5. 按上游 Windows 构建链完成源码构建及最小后台打包。
