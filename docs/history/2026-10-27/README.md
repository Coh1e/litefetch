# LiteFetch

> 2026-10-24：已拆分为独立项目。当前状态与迁移后的构建前提以 [HANDOFF.md](HANDOFF.md) 为准；协作规则见 [AGENTS.md](AGENTS.md)。下文保留历史实现说明，旧证据、旧路径与验证数字不代表本轮运行结果。依赖、组件与本机预设未随源码迁入。

当前是 Qt Widgets 开发版，已接入 libcurl、libtorrent 和 SQLite 统一任务队列。支持普通链接、BT种子及磁力链；HTTP支持暂停和强ETag校验后续传，BT支持文件选择、优先级、暂停和重新校验恢复。实验性 ed2k 已接入 Qt 设置与任务队列，支持直连源、串行多源校验拼接和完整 part 缓存恢复；Kad、回调、加密和真实网络兼容尚未完成。分段、完整做种/代理设置等仍待实现，不是成品。

BT已接统一窗口：用“打开种子”（Ctrl+O）或粘贴磁力链，选择新任务目录。种子可立即选择文件；磁力链取得元数据后弹出文件选择，确认前不开始文件数据下载。种子原始内容和选择与任务记录一同保存，移动原始种子不会使任务失效。依赖固定libtorrent2.1.2，禁用WebTorrent/WebRTC。细节见[BT接入记录](docs/research/downloader-bt-integration.md)。

浏览器集成仍在开发：MV3扩展、C++桥接、应用IPC与SQLite准备/提交已实现。工具栏“浏览器接管”已能选择目录、启停及保存本机设置，关闭后保留在途事务恢复。数据库schema4提交标记与队列任务原子保存；真实host→IPC→队列→HTTP文件已有整链测试。浏览器组件安装/注册和真实事件仍未验收，不能据此认为已能完整自动接管。详见[原生设置记录](docs/research/downloader-browser-native-settings.md)。
产品范围见 [已确认清单](docs/research/downloader-scope.md)，ed2k 当前同进程实现见 [任务接入](docs/research/ed2k-task-integration.md) 和 [Qt 设置](docs/research/ed2k-qt-entry.md)。旧 aMule 进程验证仅为归档参考，不是产品后端。

实验性 ed2k：在工具栏选择“ed2k 设置”，选择服务器或 Kad 找源，填写明确的服务器 IPv4/TCP 端口或 Kad 引导节点 IPv4/UDP 端口，勾选启用并保存；之后可通过“添加下载”输入 ed2k 文件链接。配置保存在任务目录 ed2k.json，完整校验 part 缓存在同目录 ed2k-cache，不安装或启动独立后端。当前仅支持大于零且小于 4 GiB 的文件，昵称暂限英文字符；运行中需先暂停再修改设置。Kad 已有受控端到端验证及实验性入口，长期节点维护、真实公网兼容、回调、加密和入站连接仍未完成，见 [Kad任务与Qt接入](docs/research/kad-task-and-qt-mode.md)。

2026-10-18 本机验证通过：Qt 6.11.2 的 Debug/Release 依赖已构建；C++23 Debug 程序编译部署通过。构建目录和部署目录分别在精简 PATH 下启动真实 Windows 窗口成功，均自动退出。报告与截图在 `out/verification/qt-smoke-build.json`、`qt-smoke-package.json` 及对应 PNG。

## 本机首次配置

复用现有 MSVC、CLion CMake/Ninja 和 `D:/vcpkg`。不需要单独安装 Qt SDK 或 Qt Creator。

```powershell
.\configure-local.cmd
```

此入口只在子进程中加载 Visual Studio 工具环境。vcpkg 读取固定 baseline 和最小功能清单，下载及构建 Qt；首次编译可能较久。

依赖和工具缓存：`.deps/`。程序构建：`out/build/windows-local/`。部署目录：`out/package/`。
现有 `D:/vcpkg` 用作包管理器及配方来源，不将 Qt 安装到它的共享 installed 目录；不修改全局 PATH，也不执行 vcpkg integrate install。

## 在 CLion 打开

1. 打开此目录的 `CMakeLists.txt`。
2. Toolchains 选择 Visual Studio，架构 amd64；使用 CLion 自带 CMake 与 Ninja。
3. 启用 `windows-local` CMake preset。不要选择 bundled MinGW，避免与 MSVC Qt 库混用。
4. 配置成功后，构建并运行 `native-downloader`。

`CMakeUserPresets.json` 保存本机 vcpkg/Ninja 路径，不应提交；换机器需创建自己的本机 preset，继承 `windows-base`，并设置 `VCPKG_ROOT`。
本机 preset 还配置了系统 PowerShell 下载后备入口，以避开 vcpkg 内部 TLS 连接问题；下载内容仍严格校验配方固定的 SHA-512，不关闭证书校验。
已确认用户采用开源方式；具体项目许可证尚未锁定。

## 编译与部署

本机可直接执行 `build-local.cmd`，它会在子进程中加载 MSVC 环境，编译程序并生成部署目录，不要求 CMake 已加入全局 PATH。

以下命令在已配置的 MSVC 工具链环境中执行；CMake 可以使用 CLion 内的绝对路径。

```powershell
cmake --build --preset windows-local
cmake --install out/build/windows-local --config Debug
```

构建使用 C++23、动态链接 Qt。Qt 的部署脚本负责复制运行库和平台插件。发布阶段另建 Release 配置，不向用户分发 Debug 构建。

环境冒烟验证：运行程序并传入 `--smoke-report <报告绝对路径>`。它创建实际窗口，写入 Qt 版本、平台插件、窗口可见性及下载核心接入标志，另保存窗口 PNG，然后退出。此检查不代表下载功能或完整视觉验收通过。
使用 PowerShell 7 时也可运行 `./smoke-local.ps1 -Target build` 或 `-Target package` 自动执行上述检查。

## 下一步

`test-local.cmd` 运行解析、传输和任务管理测试。传输测试只连接临时127.0.0.1 HTTP/FTP服务，验证真实二进制内容、Unicode目标路径、重定向、404、截断、取消、已有文件保护和临时文件清理，并检查真实下载经过持久化队列后的文件和完成记录。任务测试另覆盖并发上限、优先级、重复检查、存储故障及强制终止子进程后的记录恢复。受限环境可能禁止本机套接字，需要允许本地网络后运行；不能把这种连接拒绝记作通过。

libcurl 8.22.0使用Windows Schannel TLS后端，保持默认服务器证书验证。当前真实传输用例覆盖HTTP和被动FTP；HTTPS成功/证书失败、认证重定向及公网站点尚未验收。代理设置尚未接入，当前不读取进程代理环境变量。

任务数据库默认在可执行文件旁的`data/tasks.sqlite`，可用`--data-dir <目录>`指定便携数据目录；会话锁避免两个窗口同时调度同一目录。普通任务与BT共享并发队列，默认最多3个活动任务；右键可设队列优先级、打开文件/任务目录或移除记录（保留文件）。BT按完整哈希去重，hybrid的v1/v2磁力别名也会匹配已有任务。

正在运行时退出或崩溃，重启后记录为“上次运行中断”，等待显式继续。HTTP会核对已保存片段的SHA-256、强ETag、有效URL和返回范围；服务器返回完整内容时清空旧片段从头接收，缺少强ETag时安全从头下载。HTTP取消会清理本任务临时数据，BT取消保留已下载文件和恢复信息。BT“重试并校验”保留数据并核对piece；HTTP“从头重试”丢弃旧片段。FTP仍从头重试，暂不支持暂停续传。排队任务可继续调度，终态记录保留。源URL/种子存入本地数据库/恢复信息，凭据保护方案仍是发布前待办。

HTTP按已提交检查点恢复，崩溃时超出检查点的尾部会丢弃；完成文件用发布清单与本地哈希核对，处理rename后数据库尚未提交的恢复。已经实测进程强制终止与字节恢复；断电/存储硬件故障、所有文件系统边界尚未验收。分段、Last-Modified续传策略、FTP续传、跨协议队列、BT核心、ed2k适配器和浏览器集成按完整确认范围继续推进。
