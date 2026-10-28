# LiteFetch

面向 Windows 10/11 x64 的开源原生下载管理器，目标是统一支持 HTTP(S)/FTP、BT/磁力链、ed2k/Kad，以及浏览器接管、任务恢复、限速与计划任务。

项目正在从迁入的 Qt 实现重新设计为 C++23 / Win32 原生应用。当前源码仍依赖 Qt，Win32 迁移和完整功能尚未完成。原 MiniThunder 逆向成果用于行为研究，不加载其闭源下载 DLL，不接入迅雷加速网络。

## 设计与范围

外观和设计理念参考 [Figma 设计](https://www.figma.com/design/rUBzBWaWYrVZGMZlqYDCdx/lite-fetch)。主界面采用紧凑原生列表、系统菜单和控件，协议详情按需展示。与本地实现或既定需求冲突的内容须由人类确认。

完整目标和验收见 [GitHub 规格 #29](https://github.com/Coh1e/litefetch/issues/29)，已确认迁移决定见 [重设计核对](docs/product-reset.md)。阶段交付不代表完整目标完成。

## 环境准备、构建与运行

现有构建使用 MSVC、CMake/Ninja、vcpkg，以及 Qt、libcurl、libtorrent、SQLite 等依赖。旧 Qt 应用仍是完整功能的对照实现；新的无 Qt 任务核心与命令行已可用于 HTTP 下载、状态快照和持久化验证。

### 无 Qt 任务核心（当前验证范围）

前置：在项目内准备 vcpkg 并安装 curl、sqlite3（本仓库使用 `.deps/vcpkg` 与 `.deps/installed-core`，均为被忽略目录，不做全局安装；可运行 `powershell -File scripts\bootstrap-core-deps.ps1`）。先加载 MSVC 环境（如 `vcvars64.bat`），然后：

```powershell
cmake --preset windows-core
cmake --build out/build/windows-core
ctest --test-dir out/build/windows-core --output-on-failure
```

`windows-core` 预设显式关闭 Qt 对照目标、使用项目内最小依赖树，并保持 `TEMP`/`TMP` 在项目内。命令行入口：

```powershell
out/build/windows-core/litefetch-core-cli.exe add --db data/tasks.db --source http://127.0.0.1:8080/file.bin --destination D:\Downloads\file.bin
out/build/windows-core/litefetch-core-cli.exe start --db data/tasks.db --id <task-id>
out/build/windows-core/litefetch-core-cli.exe list --db data/tasks.db
```

手工验证可用 `node scripts/core-http-fixture.mjs 8080 262144` 启动受控 HTTP 对端；脚本输出内容 SHA-256，便于与下载文件独立比对。

### 旧 Qt 对照路径

`windows-base` 预设和 `native-downloader` 目标保留原有 Qt 实现，依赖 Qt/libtorrent 等完整依赖树；默认在检测到 Qt 时启用，也可用 `-DLITEFETCH_BUILD_QT_LEGACY=ON/OFF` 显式控制。本轮未构建该路径。

旧入口 configure-local.cmd、build-local.cmd、test-local.cmd 和 smoke-local.ps1 保留供迁移核对；其中含旧机器工具路径，并依赖未迁入的本机预设和依赖，当前不能保证直接运行。准备环境前先核对这些路径和 CMakePresets.json，不将历史构建成功视为当前可构建。

依赖和缓存放 .deps/，构建和部署放 out/；本机 CMakeUserPresets.json 不入库。工具、配置、临时文件与运行数据均留在项目内，不做全局安装或使用其他项目配置。

迁入版本以 native-downloader 为程序目标，支持 --data-dir 指定任务数据目录。此处描述现有入口，不表示 Win32 版本已交付。旧入口本轮未运行构建或功能验证；无 Qt 核心的验证见上节。

## 协作与历史

任务与规格使用 [GitHub Issues](https://github.com/Coh1e/litefetch/issues)，协作和执行分工见 [AGENTS.md](AGENTS.md)。

迁移前的 README、交接和工作流见 [历史快照](docs/history/2026-10-27/README.md)。docs/research/ 保留技术研究与历史证据，按任务读取；旧范围、暂停状态、Qt 选择和历史验证数字不作为新任务的默认指令或本轮验收。

## 许可证

开源方向已确认；项目许可证及依赖分发材料仍待核对和确定。
