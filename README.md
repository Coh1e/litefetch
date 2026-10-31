# LiteFetch

LiteFetch 是面向 Windows 10/11 x64 的开源原生下载管理器。它统一支持 HTTP(S)、FTP、BT/磁力链和 ed2k/Kad，并提供浏览器接管、任务恢复、限速、计划任务、托盘菜单和完成通知。

当前实现是 **C++23 / Win32 原生应用**。Qt 旧实现已从产品源码移除；`tests/` 中的 Qt 旧测试源不参与构建。原 MiniThunder 逆向成果只用于行为研究：本项目不加载它的闭源下载 DLL，也不接入迅雷加速网络。

交付入口：

- [最终交代](docs/specs/final-report.md)：全范围结论、逐票状态与未验证边界；
- [验证索引](docs/specs/verification-index.md)：测试与票面的对应；
- [人工验收清单](docs/specs/human-acceptance-checklist.md)：需要人完成的步骤。

## 设计与范围

外观和设计理念参考 [Figma 设计](https://www.figma.com/design/rUBzBWaWYrVZGMZlqYDCdx/lite-fetch)。主界面使用紧凑原生列表、系统菜单和控件。协议详情不常驻主界面。设计内容与本机实现或已确认的产品需求冲突时，由人类确认。

完整目标和验收见 [GitHub 规格 #29](https://github.com/Coh1e/litefetch/issues/29)。已确认的迁移决定见 [重设计核对](docs/product-reset.md)。阶段交付不表示完整目标已完成。

## 环境准备、构建与运行

构建使用 MSVC 和 CMake/Ninja。依赖（libcurl、SQLite、Boost、libtorrent、nlohmann/json）由 CMake 的 FetchContent 按固定版本从源码拉取并编译。本项目不需要 vcpkg 或其他系统级包管理器。

### 构建、测试与运行

先加载 MSVC 环境（`vcvars64.bat`），然后运行：

```powershell
cmake --preset windows-core
cmake --build out/build/windows-core
cmd /c test-local.cmd     # 24 个测试，约 3 分钟（本机 138–185 秒）
out\build\windows-core\litefetch-win32.exe
```

首次配置会下载并编译依赖。libtorrent 和 Boost 耗时最多。所有依赖落在 `out/build/<preset>/_deps` 内，不写入系统目录。`TEMP` 和 `TMP` 保持在项目内。

命令行入口：

```powershell
out/build/windows-core/litefetch-core-cli.exe add --db data/tasks.db --source http://127.0.0.1:8080/file.bin --destination D:\Downloads\file.bin
out/build/windows-core/litefetch-core-cli.exe start --db data/tasks.db --id <task-id>
out/build/windows-core/litefetch-core-cli.exe cancel --db data/tasks.db --id <task-id>
out/build/windows-core/litefetch-core-cli.exe list --db data/tasks.db
```

发布包：`powershell -File scripts/package-release.ps1` → `out/package/litefetch-0.1.0`。
包内 smoke：`powershell -File scripts/win32-smoke.ps1 -AppPath <exe>`。

手工验证可用 `node scripts/core-http-fixture.mjs 8080 262144` 启动受控 HTTP 对端。脚本输出内容 SHA-256，便于与下载文件独立比对。

### 一次构建与产物（本地与 CI 相同）

本地运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\ci.ps1
```

脚本自动加载 MSVC 环境，然后配置、构建、运行 24 个测试并打包。产物固定在：

| 路径 | 内容 |
|---|---|
| `out/ci/artifacts/litefetch-0.1.0-win64.zip` | 便携包，解压后运行 `litefetch-win32.exe` |
| `out/ci/artifacts/SHA256SUMS.txt` | 产物校验值 |
| `out/ci/summary.md` | 本次结果：状态、文件大小、SHA-256、日志位置 |
| `out/ci/manifest.json` | 同一结果的机器可读版本 |
| `out/ci/logs/` | 每一步的日志 |

GitHub Actions 运行同一个脚本（`.github/workflows/windows.yml`）：

- 打开 Actions → Windows → 最近一次运行，在 Artifacts 中下载 `LiteFetch-Windows-x64`（构建产物）和 `LiteFetch-logs`（日志）。运行摘要页显示同一份 `summary.md`。
- 推送 `v*` tag 时，同一份 zip 和校验值自动附加到 Release。
- 发布前冒烟用 `-Smoke`；tag 构建自动启用。

### 浏览器接管

扩展在 `browser/`。宿主注册清单在 `out/browser-connection/`，宿主程序由构建生成在 `out/build/windows-core/`。安装步骤与注册键见 [人工验收清单](docs/specs/human-acceptance-checklist.md) 第 4 节。真实 Chrome 下的联调记录见 [浏览器联调](docs/research/browser-handoff-real-chrome.md)。

### 目录与数据

依赖和缓存放在 `.deps/`，构建和部署放在 `out/`。开发和验收产生的临时文件与日志留在项目内。本机 `CMakeUserPresets.json` 不入库。本项目不做全局安装，也不使用其他项目的配置。

产品默认把任务数据库放在 `%LOCALAPPDATA%\LiteFetch\tasks.db`，把下载保存到用户的「下载」文件夹。用 `--db` 和 `--download-dir` 可改到指定路径。启动时若该目录不可写，程序回退到 exe 旁的 `data\` 和 `downloads\`。

迁移前的构建入口（configure-local.cmd、build-local.cmd、smoke-local.ps1）已从仓库移除，只在 [历史快照](docs/history/2026-10-27/README.md) 中保留说明。不要按历史路径构建。

CMake 工程名仍为 NativeDownloader；可执行文件和发布包使用 litefetch 前缀。

## 协作与历史

票与规格使用 [GitHub Issues](https://github.com/Coh1e/litefetch/issues)。协作和执行分工见 [AGENTS.md](AGENTS.md)。

迁移前的 README、交接和工作流见 [历史快照](docs/history/2026-10-27/README.md)。旧状态文档归档在 [2026-10-30](docs/history/2026-10-30/)，只用于追溯。docs/research/ 保存技术研究和历史证据，按票读取。旧范围、暂停状态、Qt 选择和历史验证数字不是新票的默认指令，也不是本次验收的依据。

## 许可证

MIT，见 [LICENSE](LICENSE)。第三方依赖的许可清单随发布包提供（`THIRD-PARTY.txt`）。
