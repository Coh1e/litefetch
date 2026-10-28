# LiteFetch 交接

日期：2026-10-24。状态：下载器目标未完成，保持暂停。本轮从 downloader 整理为独立项目并初始化 Git，没有提交或推送。

## 工程技能配置（2026-10-24）

已按人类确认配置 GitHub Issues（Coh1e/litefetch）、五个默认分诊标签及 single-context 领域文档布局。入口为 AGENTS.md 的 Agent skills，细则在 docs/agents/issue-tracker.md、triage-labels.md、domain.md。GLOSSARY.md 与 docs/adr/ 按需创建；本轮未创建空文档。此配置不恢复开发，也不改变 OpenPi 执行分工。

实际验证：读取模板、现有项目约定及 Git 远端；浏览器查看 GitHub 标签页，wontfix 已存在，needs-triage、needs-info、ready-for-agent、ready-for-human 尚不存在。本机当前找不到 gh 命令，内置浏览器未登录，连接器无创建仓库标签接口，因此远端标签配置尚未完成。文档静态检查与 git diff --check 结果见本轮报告。构建、功能测试、逆向：未运行；历史 41/41 与 230 项静态核查仍仅为下文历史证据。

文档验证结果：Agent skills 入口仅一处，三个配置文件存在，配置文件行尾空白检查通过，git diff --check 通过。

后续实测（2026-10-24）：用户安装 gh 后已重新检查，当前可用版本为 2.103.2，入口为 C:/Users/Neo/scoop/shims/gh.exe；gh auth status 返回尚未登录任何 GitHub 主机，gh label list 因未认证未能读取。前述“找不到 gh”是安装前状态，当前剩余阻塞为认证。未创建远端标签，未修改凭据或全局环境。

最终实测（2026-10-24）：允许联网后，gh auth status 确认 Coh1e 登录有效；先前受限环境的连接失败不代表凭据失效。已在 Coh1e/litefetch 成功创建 needs-triage、needs-info、ready-for-agent、ready-for-human；再次 gh label list 核验五个默认标签全部存在（wontfix 复用原有标签）。工程技能初始化完成。

下一步：按需使用工程技能，直接维护 docs/agents/*.md；产品开发继续保持暂停，恢复顺序见下文。未提交或推送此次配置。构建、功能测试、逆向：未运行。

## 目标与当前实现

Windows、Qt 6 Widgets、C++23、CMake/vcpkg；Everything 式紧凑列表与键盘操作。HTTP(S)/FTP、libtorrent BT/磁力、SQLite 队列与恢复、实验性同进程 ed2k/Kad、Chrome 扩展及 Native Messaging 交接已有实现。完整范围见 docs/research/downloader-scope.md；最新暂停现场见 docs/research/downloader-paused-handoff.md，进度见 docs/research/downloader-progress.md。

不做迅雷加速/P2SP、不复用闭源迅雷 DLL；aMule 仅为历史实验对照。依赖保留 libtorrent 与最小 Boost 闭包，不引入全量 Boost、aria2 或 wxWidgets。

## 停点与恢复顺序

最近产品回归历史基线为 41/41，通过记录在本机归档 logs/downloader-tests-upload-metrics-full.log；之后只改逆向资料，不能据此认为全部范围完成。

浏览器原引擎静态 PE 字节核查为 230 项，runtimeVerified=false。最新方向追踪全局管理器 +28 的 10037460：10375a94 操作表、33339999 分派与载荷 +a0/+a4/+a8 的读取者。最后发现尚未全部写入字节断言，勿重新从头扫描。详见 docs/research/browser-downstream-numbered-factory.md。

人类恢复后，先核对上述分派链与错误事件的真实任务身份，再核对接收者配对、取消/迟到回调与文件一致性。原版浏览器自动接管动态对照仍待进行。产品缺口包括分段与连接复用、跨核心限速、代理/TLS、ed2k 上传/LowID/大文件、Kad 维护与互通、独立做种队列、计划任务、Edge/Firefox、认证与许可证审计；以完整范围文档为准。

## 构建与迁移影响

源码和内部目标名仍为 native-downloader，产品名采用 LiteFetch；本轮不修改业务代码。构建入口为 configure-local.cmd、build-local.cmd，测试为 test-local.cmd。工具路径仍针对原机器的 MSVC/CLion/SDK，需要人工确认。

.deps、out、cmake-build-debug、IDE 配置及 CMakeUserPresets.json 已留在本机归档，不进入新项目。windows-local 是私有预设，清洁检出时尚不存在；必须按 CMakePresets.json 的 windows-base 在项目内重新创建本地预设并安装依赖，不能直接执行构建入口。旧预设引用 D:/vcpkg，本轮没有复制或启用该配置。

依赖核对脚本已收进 scripts/；aMule 实验入口只指向项目 tools/，工具尚未提供。旧逆向脚本保存在归档，继续分析前需由人类按需提供并检查其路径，不直接运行旧绝对路径配置。

## 归档与证明边界

本机 D:/Projects/rea/archive/legacy-workspace 保存原 downloader/、analysis/、scripts/、target/、tools/、work/、logs/。原配置和证据中的 downloader/ 或 analysis/ 路径均指旧工作区；本项目的研究笔记是历史快照。

OpenPi、DeepSeek、rea、桥接和技术沙箱均未配置。本轮仅完成文件哈希、路径和 Git 检查，未重新构建或运行功能测试。完整许可证选择和第三方分发审计仍未完成。
