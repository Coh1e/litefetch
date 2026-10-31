# Win32 LiteFetch 最终交代

日期：2026-10-31（#33 收口后复核）。本文回答一件事：**哪些做完了、哪些没有、证据在哪、还需要人做什么。**

## 一、验收方式与最近一次结果

```
cmd /c test-local.cmd        (exit code 0)
→ 100% tests passed out of 24
→ Total Test time (real) = 139.25 s   (2026-10-31，#33 收口复跑)
```

- 24 个测试全部在进程内运行（跨进程 e2e 已由 ADR-0001 取消）；
- 测试与票面的对应关系见 `docs/specs/verification-index.md`，已知偶发见 `docs/specs/known-flakes.md`；
- 发布包：`out/package/litefetch-0.1.0`，5 个文件 / 5.5 MiB（全静态）；
- 包内 smoke：窗口 219 ms 出现、退出码 0（`.pi/evidence/final/`）。

## 二、逐票状态

### 已完成（16 张，Issue 已关闭）

| 票 | 主题 | 关键证据 |
|---|---|---|
| 04 | 列表、批量入口与基本任务操作 | `ui-model`、`ui-view`、`ui-batch` |
| 05 | HTTP 暂停、续传与崩溃恢复 | `core-service`、`core-store`；强杀重启实验（`docs/research/blocked-storage-and-forced-kill.md`） |
| 06 | HTTPS 与站点认证 | `core-service`、`core-credentials` |
| 07 | HTTP 分段、连接复用与站点上限 | `core-service` |
| 08 | FTP 下载、认证与续传 | `core-service`（FTP fixture） |
| 09 | BT/磁力真实下载与文件确认 | `core-bt` |
| 10 | BT 属性、文件优先级与追踪器 | `ui-window`、`core-bt` |
| 11 | BT 恢复、校验、上传与做种 | `core-bt` |
| 12 | ed2k 服务器找源与同进程下载 | `core-ed2k` |
| 13 | ed2k 大文件、多源与发布恢复 | `core-ed2k` |
| 14 | Kad 查找、路由与持久恢复 | `core-kad` |
| 15 | ed2k 上传、入站、LowID 与协议互通 | `core-ed2k` |
| 19 | SHA-256 验证及统一错误恢复 | `core-service`（含中文失败文案） |
| 21 | 凭据按需记住与隐私验证 | `core-credentials`（DPAPI，不落明文） |
| 22 | 现代浏览器方案与真实接管边界 | `docs/research/browser-handoff-real-chrome.md` |
| 27 | 移除 Qt、Release 包与依赖/许可证审计 | `scripts/package-release.ps1`、`LICENSE`、`THIRD-PARTY.txt` |

### 部分完成（5 张，Issue 打开）

| 票 | 已交付 | 缺口 |
|---|---|---|
| 16 | 跨引擎限速、并发槽、优先级、低占用、下载/上传/校验三类流量 | **元数据流量**未计：libtorrent 不通过进度回调报告元数据字节 |
| 17 | 选项对话框可达（限速/代理/超时/ed2k/并发/时段/计划），代理经 `CURLOPT_PROXY` | 对话框交互**无自动化验证**：模态窗口，进程内 harness 驱不动 |
| 18 | 删除任务与文件、空间预检、同名策略、分类保存、暂停保留进度；受阻目录与强杀一致性已有真实证据 | 实验用"拒绝写入的目录"而非只读卷（表现一致，介质不同）；等你确认后关闭 |
| 25 | 时段限速、计划任务、完成动作；"完成后关闭程序"已由 `ui-window` 锁住（含有牙检查） | 只剩"完成后打开文件夹"（会拉起资源管理器，无法在进程内断言） |
| 26 | 托盘、通知、DPI（含 `WM_DPICHANGED` 重排用例）、加速键审计 | 高对比目视、纯键盘走查、逐状态画板核对 |

### 未完成（4 张，Issue 打开）

| 票 | 状态 | 原因 |
|---|---|---|
| 20 | 未开工 | 你确认本机没有旧库样本，导入两种数据模式无从验证 |
| 23 | **协议层与 host 已真机验证，点击接管未验证** | 真实 Chrome 下，扩展 → host → 管道 → 应用 → 建任务可用；"点击下载 → 暂停 → 交接 → 提交"未跑通，详见第七节 |
| 24 | 未开工 | 依赖 23；且需要 Edge 与 Firefox 的实机验证 |
| 28 | 未完成 | 干净 Windows 全范围验收，本环境不具备条件 |

## 三、本会话修掉的真实缺陷

不是为了让测试变绿而改的代码，每条都有失败现象与验证方式：

1. **浏览器 host 读帧两个致命 bug**：stdin/stdout 未设二进制模式（长度字节被翻译）、按 4096 字节块读导致永久阻塞。真实 Chrome 联调立刻暴露，已加管道回归用例（写一帧并保持写端打开）；
2. **租约字段错误**：扩展用 `expiresAt` 与 `Date.now()` 比较，桥接层只回 `expiresInMs` → 判 `native_lease_invalid`；
3. **观察器误判**：同源 `Referer` 被当成不可重放；导航转下载的 `ERR_ABORTED` 被当成请求失败；
4. **设置不持久**：`TaskWorker` 从不调用 `loadSettings()`，重启后限速、代理、并发、时段、计划、完成后动作**全部回默认**；
5. **批次记账竞态**：`jobs_.push_back()` 与 `batchActive_ = true` 不在同一把锁内，快速传输下"批次结束"事件永不产生；
6. **错误文案混语言**：引擎英文错误直接显示在中文界面，现为"无法写入目标文件（destination directory does not exist）"。

第 4、5 条是从"完成后动作不触发"一条线索挖出来的，影响面远超该功能。

## 四、只用真机才验证到的三件事

| 事项 | 结果 | 位置 |
|---|---|---|
| 扩展 → native host → 管道 → 应用 → 建任务 | `prepare` 返回 `ok:true`，任务落库 | `docs/research/browser-handoff-real-chrome.md` |
| 受阻存储 | 目录拒绝写入 → 任务 `failed`、`error: cannot create the partial file`、无重试 | `docs/research/blocked-storage-and-forced-kill.md` |
| 强制终止 | 强杀后库中 `running`，重启变 `paused` | 同上 |

## 五、交付物与代码位置

```
src/core/      下载引擎、任务服务、调度、限速、流量、凭据、BT/ed2k/Kad、浏览器桥接
src/win32/     窗口、列表模型、选项、托盘、后台 worker
src/tools/     litefetch-core-cli、litefetch-browser-host
browser/       MV3 扩展（manifest、worker、适配器、弹窗）
tests/         24 个测试的源码
docs/specs/    交付状态、验收报告、人工验收清单、票面计划、验证索引
docs/research/ 浏览器联调、受阻存储与强杀、完成动作排查、Figma 对照
scripts/       打包、smoke、浏览器联调探针
```

## 六、需要你做的三件事

1. **#23（1 分钟）**：`chrome://extensions` → 开发者模式 → 加载已解压的扩展程序 → 选 `browser/`；
   确认注册键指向 `out/browser-connection/org.native_downloader.bridge.json`（UTF-8 无 BOM）；
   启动 LiteFetch，在弹窗里打开开关并允许站点，点一个下载链接，看本机列表是否出现任务。
   **出现了 → #23 通过；没出现 → 把现象告诉我（弹窗提示、徽标是否变 `!`）。**
2. **#18**：证据已齐，你点头我就关闭。
3. **#28**：需要一台干净 Windows，或你明确接受"单机验证"这个边界。

## 七、为什么 #23 的最后一环没跑通

真实 Chrome 下已确认：扩展加载成功、native 通道可用、请求观察记录有效（`invalid:false`、有请求头与 200 响应）、
下载事件到达（`state: in_progress`）。卡在匹配器给出的 `browser_not_eligible`，而同一时刻读到的下载项是
`in_progress / paused:false / byExtensionId:null / incognito:false`——按判定条件应当合格，两者矛盾。

已排除：通道、租约字段、清单 BOM、HKCU 注册、监听器注册（`hasListeners()` 全为 true）、服务端限速。

自动化手段的边界：SW 里 `import()` 被规范禁止；`ServiceWorker.startWorker` 在 Chrome 155 已移除；
`--load-extension` 开关被特性关闭。这些都不是产品缺陷，靠继续折腾 CDP 收益很低。
**所以这一步留给你手工一分钟，是当前最有效的路径。**

## 八、关闭后的最终状态（2026-10-30）

按用户决定，#04–#22、#24–#28 全部关闭。

| 票 | 结论 |
|---|---|
| 04–15、19、21、22、27 | 实现、测试与验收记录齐备 |
| 16 | 元数据桶已接入并有确定性用例；引擎侧计数为尽力而为 |
| 17 | 设置跨重启有守门用例（含有牙检查）；对话框交互需人眼 |
| 18 | 受阻目录与强杀今日复测通过 |
| 20 | 未实现（无旧库样本），按决定关闭 |
| 24 | Edge 已注册同一清单；Firefox 与认证接管未做 |
| 25 | 关闭程序与打开文件夹均有用例（后者断言决定与目标目录） |
| 26 | DPI/加速键有用例；界面无硬编码配色；目视与键盘走查需人 |
| 28 | 单机全范围复核通过；**干净 Windows 未验证**（边界已随票记录） |

**唯一保持打开的功能性缺陷是 #23（浏览器接管）**，用户判定为重大缺陷，不随本次收尾关闭。

复核数字：`cmd /c test-local.cmd` → 23/23 通过，166.2 s；发布包 5 文件 / 5.5 MiB，
包内 smoke：窗口 376 ms 出现、退出码 0（`.pi/evidence/close/`）。

## 九、收口（2026-10-30）

#29 与 #23 按所有者决定关闭，**全部 28 张票均为关闭状态**，其中 #23 是关闭但**未修复**的重大缺陷。

### 交付事实

- `cmd /c test-local.cmd` → 23/23 通过，最近 163.75 s；
- 发布包 5 文件 / 5.5 MiB，全静态；包内 smoke 窗口 184 ms、退出码 0；
- 依赖由 CMake FetchContent 从源码构建，无 vcpkg；
- 推送护栏：构建 + 二进制启动检查 + vcpkg 残留检查 + 整套测试。

### 关闭时的边界（不因关闭而消失）

1. 干净 Windows 全范围验收**未做**；
2. 浏览器接管**不工作**（#23）；
3. Firefox 与认证接管**未实现**（#24 的一部分）；
4. 旧任务导入**未实现**（#20，无样本）；
5. 真实公网 BT/Kad/eD2k **未验证**；
6. 高对比目视、纯键盘走查、Figma 页面 02+ 核对**需人**。

### 已知偶发

见 `docs/specs/known-flakes.md`：`core-bt`（端口根因已修，12/12 绿，挂死留阶段日志）、
`ui-window`（19 次未复现，选行竞态已修）。

## 十、2026-10-31 复审补记

本轮复审后修复并提交：

- 重新校验的 `forceVerify` 现在穿过 worker 到达引擎；已完成 BT 任务允许强制重查；
- schema 升级改为按列补齐，v2–v8 都能升到 v9（v2/v6/v8 有回归用例）；
- 扩展的日志键前缀由 `JOURNAL_PREFIX` 单点定义，弹窗与 e2e 不再过滤过期前缀；
- 工具栏“选项”可用、文案改为“打开文件夹”、Shift+Delete 走删除文件确认、能力说明与关于框去掉过期句子。

新增 `scripts/ci.ps1` 与 `.github/workflows/windows.yml`：本地与 CI 跑同一条流水线，产物、摘要与日志固定在 `out/ci/`。本轮 Release 全套 23/23。

2026-10-31 首次 GitHub Actions 运行成功（run #8，`b43999e`），产物 `LiteFetch-Windows-x64` 与日志可下载。此前的 D8050（`cannot execute c1.dll`）根因是 `CMakePresets.json` 把 `TEMP/TMP` 指向 `.deps/tmp`，干净检出没有这个目录；`scripts/ci.ps1` 与 `test-local.cmd` 现在先创建它。

文档真源收口到本文；`win32-delivery-status.md`、`win32-acceptance-report.md`、`win32-ticket-plan.md`
移入 `docs/history/2026-10-30/`，只作追溯。

新开的票：

| 票 | 主题 | 状态 |
|---|---|---|
| #33 | 凭据对话框没有把“记住密码”传给存储 | **完成**：对话框抽为独立模块并读取 `IDC_CRED_REMEMBER`；勾选/未勾选、DPAPI 重载、重启后 401 重试均有用例 |
| #34 | 托盘菜单与完成通知未接通 | 待做；需要真实桌面验收 |
| #35 | `core-service` 偶发挂起，ctest TIMEOUT 未生效 | 实现完成；十轮 soak 9/10（第 10 轮为 `core-bt` 已知 TIMEOUT） |
| #36 | 门禁：脚本语法/编码与项目内路径检查 | 完成：两道检查接入 pre-push 与 CI |
| #37 | 首次在 GitHub Actions 运行 Windows 流水线 | 完成：两次运行与 tag Release 均绿 |
| #38 | 清理旧测试与死脚本，加 clean/doctor | 待做（删除部分已完成） |
| #39 | 命令表驱动，删除列宽双实现 | 待做 |
| #40 | 剩余 UI 差距：属性页、DPI 图标、上传段与字体 | 待做 |
| #41 | 结构重构剩余项：对话框样板、ed2k/HTTP 工具、测试夹具与 CMake | 待做 |
| #42 | 机器文档收口与离线设计基线 | 待做 |

旧库升级策略记录在 [ADR-0002](../adr/0002-column-driven-schema-upgrades.md)：保留全部行，按缺失列补齐到 v9。
剩余票的技术取舍与顺序见 [收尾规格](win32-remaining-work.md)。

2026-10-31 #33 收口：`credential_dialog.{h,cpp}` 移出 `main_window.cpp`，生产路径仍是模态对话框，测试用同一资源与过程创建非模态实例。
新增 `ui-credential`（勾选写 DPAPI、未勾选不落盘、取消不交出凭据、提示文案与复选框一致、密码框掩码）；
`ui-window` 增加持久化凭据后新窗口自动重试已保存 401 的场景。验收 `cmd /c test-local.cmd` → 24/24 通过，139.25 s，退出码 0；日志在 `tmp/acceptance.log`。
边界：跨 Windows 账户的实际重认证未自动验证（单机无法制造第二账户），DPAPI 用户作用域与篡改 blob 由 `core-credentials` 覆盖；未新增凭据日志。
