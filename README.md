# LiteFetch

面向 Windows 10/11 x64 的开源原生下载管理器，目标是统一支持 HTTP(S)/FTP、BT/磁力链、ed2k/Kad，以及浏览器接管、任务恢复、限速与计划任务。

当前实现是 **C++23 / Win32 原生应用**，Qt 旧实现已从仓库移除。逐票交付状态见
[交付状态](docs/specs/win32-delivery-status.md)，全范围结论见 [最终交代](docs/specs/final-report.md)，
测试与票面的对应见 [验证索引](docs/specs/verification-index.md)。
原 MiniThunder 逆向成果仅用于行为研究，不加载其闭源下载 DLL，不接入迅雷加速网络。

## 设计与范围

外观和设计理念参考 [Figma 设计](https://www.figma.com/design/rUBzBWaWYrVZGMZlqYDCdx/lite-fetch)。主界面采用紧凑原生列表、系统菜单和控件，协议详情按需展示。与本地实现或既定需求冲突的内容须由人类确认。

完整目标和验收见 [GitHub 规格 #29](https://github.com/Coh1e/litefetch/issues/29)，已确认迁移决定见 [重设计核对](docs/product-reset.md)。阶段交付不代表完整目标完成。

## 环境准备、构建与运行

现有构建使用 MSVC、CMake/Ninja 与项目内 vcpkg（libcurl、libtorrent、SQLite 等）。

### 构建、测试与运行

前置：在项目内准备 vcpkg 依赖（本仓库使用 `.deps/`，被忽略，不做全局安装；可运行
`powershell -File scripts\bootstrap-core-deps.ps1`）。先加载 MSVC 环境（`vcvars64.bat`），然后：

```powershell
cmake --preset windows-core
cmake --build out/build/windows-core
cmd /c test-local.cmd     # 23 个测试，约 165–185 秒
out\build\windows-core\litefetch-win32.exe
```

`windows-core` 预设使用项目内最小依赖树，并保持 `TEMP`/`TMP` 在项目内。命令行入口：

```powershell
out/build/windows-core/litefetch-core-cli.exe add --db data/tasks.db --source http://127.0.0.1:8080/file.bin --destination D:\Downloads\file.bin
out/build/windows-core/litefetch-core-cli.exe start --db data/tasks.db --id <task-id>
out/build/windows-core/litefetch-core-cli.exe list --db data/tasks.db
```

发布包：`powershell -File scripts/package-release.ps1` → `out/package/litefetch-0.1.0`；
包内 smoke：`powershell -File scripts/win32-smoke.ps1 -AppPath <exe>`。

手工验证可用 `node scripts/core-http-fixture.mjs 8080 262144` 启动受控 HTTP 对端；脚本输出内容 SHA-256，便于与下载文件独立比对。

### 浏览器接管

扩展在 `browser/`，原生宿主在 `out/browser-connection/`。安装步骤与注册键见
[人工验收清单](docs/specs/human-acceptance-checklist.md) 第 4 节；真实 Chrome 下的联调记录见
[浏览器联调](docs/research/browser-handoff-real-chrome.md)。

### 旧入口

configure-local.cmd、build-local.cmd、test-local.cmd 与 smoke-local.ps1 中的工具路径属于迁移前环境；
准备环境前先核对它们与 `CMakePresets.json`，不将历史构建成功视为当前可构建。

依赖和缓存放 .deps/，构建和部署放 out/；本机 CMakeUserPresets.json 不入库。工具、配置、临时文件与运行数据均留在项目内，不做全局安装或使用其他项目配置。

迁入版本以 native-downloader 为程序目标，支持 --data-dir 指定任务数据目录。此处描述现有入口，不表示 Win32 版本已交付。旧入口本轮未运行构建或功能验证；无 Qt 核心的验证见上节。

## 协作与历史

任务与规格使用 [GitHub Issues](https://github.com/Coh1e/litefetch/issues)，协作和执行分工见 [AGENTS.md](AGENTS.md)。

迁移前的 README、交接和工作流见 [历史快照](docs/history/2026-10-27/README.md)。docs/research/ 保留技术研究与历史证据，按任务读取；旧范围、暂停状态、Qt 选择和历史验证数字不作为新任务的默认指令或本轮验收。

## 许可证

MIT，见 [LICENSE](LICENSE)。第三方依赖的许可清单随发布包提供（`THIRD-PARTY.txt`）。
