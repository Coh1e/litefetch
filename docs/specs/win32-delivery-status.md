# Win32 交付状态与证据

对照 `docs/specs/win32-ticket-plan.md` 与父规格 Issue #29，逐票记录当前状态、证据与缺口。
更新时间以最后一次提交为准；验收命令统一为 `cmd /c test-local.cmd`（当前 17 个测试，约 137 秒，全部通过）。

状态含义：

- **完成**：票面交付与验收项都有本次运行的证据，Issue 已关闭；
- **部分**：主体已交付，仍有一到两项未做或无环境验证，Issue 保持打开；
- **未开工**：没有实现，Issue 保持打开。

## 一、完成（Issue 已关闭）

| 票 | 主题 | 关键交付 | 证据 |
|---|---|---|---|
| 01 | 项目本地执行与证据准备 | 全部产物在项目内（`.deps`/`out`/`tmp`），单一验收命令 | `test-local.cmd`、AGENTS.md |
| 02 | 命令与快照边界的最小验证 | `litefetch-core`：命令/快照/持久化分离 | `core-service`、`core-store` |
| 03 | 新建 HTTP 下载与真实列表 | 新建对话框、列表模型、worker 线程 | `ui-window`、`ui-model` |
| 04 | 列表、批量入口与基本任务操作 | 批量入口、右键菜单、按名选中 | `ui-window`、`ui-batch` |
| 05 | HTTP 暂停、续传与崩溃恢复 | `.lfpart`+`.lfpart.meta`，重启续传 | `core-service`、`ui-window` |
| 06 | HTTPS 与站点认证 | TLS + 401 询问凭据 + 重试 | `core-service` |
| 07 | HTTP 分段、连接复用与站点上限 | 分段 Range、每站点并发上限 | `core-service` |
| 08 | FTP 下载、认证与续传 | FTP REST 续传、登录 | `core-service`（ftp fixture） |
| 09 | BT/磁力真实下载与文件确认 | libtorrent、选择文件、磁力元数据 | `core-bt`、`ui-window` |
| 10 | BT 属性、文件优先级与追踪器 | 五页属性、优先级、trackers | `core-bt`、`ui-window` |
| 11 | BT 恢复、校验、上传与做种策略 | 断点恢复、逐片校验、真实上传、做种预算 | `core-bt` |
| 12 | ed2k 服务器找源与同进程下载 | 服务器源查询、同进程下载 | `core-ed2k` |
| 13 | ed2k 大文件、多源与发布恢复 | 多源切换、分块发布恢复、自组网验证 | `core-ed2k` |
| 14 | Kad 查找、路由与持久恢复 | id/路由/线格式/UDP 节点/来源发布查询/接任务 | `core-kad`、`core-ed2k` |
| 17 | 系统/自定义代理与超时设置 | 选项对话框、代理经 `CURLOPT_PROXY` | `core-service`（夹具收到绝对 URL） |
| 19 | SHA-256 验证及统一错误恢复 | 校验值输入/校验/重新校验、重试预算 | `core-service`、`ui-window` |
| 21 | 凭据按需记住与隐私验证 | DPAPI 存储、自动重试、清除入口 | `core-credentials` |
| 22 | 现代浏览器方案与真实接管边界验证 | 平台事实与推断分离的研究文档 | `docs/research/browser-takeover.md` |

## 二、部分（Issue 打开）

| 票 | 已交付 | 缺口 | 原因 |
|---|---|---|---|
| 15 | 入站服务、任务级共享、只服务任务数据、暂停/停止终止共享、上传计数 | LowID/回调、扩展与加密兼容、真实客户端双向 | 需要真实 eD2k 服务器与网络，本环境不可得 |
| 16 | 跨引擎共享令牌桶（HTTP/FTP 已接）、全局限速 | ed2k/BT 未接、优先级、并发槽、分类计数、低占用 | 时间；BT 需 libtorrent 设置，ed2k 需改自身循环 |
| 18 | 删除任务与文件、空间预检、同名策略、分类保存、暂停保留进度 | 真实受阻卷与强杀进程两类证据 | 环境无法制造 |
| 26 | 托盘图标/菜单/双击、完成通知（策略可测）、DPI 感知与 `WM_DPICHANGED` | 高对比验证、键盘全路径审计、UIA 全量审计、逐状态画板核对 | Figma 页面 02 之后读不到；时间 |
| 27 | Release preset、便携包脚本、依赖闭包、README/THIRD-PARTY、**包内 exe 真实启动 smoke 通过** | 删除 160 个 Qt 遗留源文件、产品许可证 | 待人类确认删除范围与许可证 |

## 三、未开工（Issue 打开）

| 票 | 主题 | 说明 |
|---|---|---|
| 20 | 旧任务显式导入与两种数据模式 | 需要旧库样本与迁移规格确认 |
| 23 | 浏览器本机交接与 Chrome 完整链 | 需要 Chrome 与扩展安装环境 |
| 24 | Edge/Firefox 与认证接管、组件生命周期 | 同上，且依赖 23 |
| 25 | 计划、时段限速与完成动作 | 可做，但窗口不足 |
| 28 | 全范围总验收与干净 Windows 交付验证 | 依赖上面全部完成 |

## 四、贯穿性事实

- **验收方式**：`cmd /c test-local.cmd`，17 个测试，全部在进程内运行；跨进程 e2e 已被 ADR-0001 取消（会触发 SmartScreen 提示，且人类明确要求不关闭 SmartScreen）。
- **环境限制**（本会话）
  - 通知区与剪贴板不可用：托盘气泡、复制链接的断言会打印跳过说明；
  - 无公网 Kad/eD2k 节点：真实网络互通一律未验证；
  - Figma 只读到页面 01：后续画板无法逐状态核对；
  - `web_fetch` 被 DNS 策略拦截，改用 PowerShell 抓取。
- **已知测试注意事项**：不要绕过 `test-local.cmd` 单跑测试——它把 `TEMP/TMP` 指向项目内 `.deps\tmp`，沙箱化进程写不了系统临时目录。

## 五、距离 #28 的要求

#28 要求"全范围复核、干净 Windows 上验证交付"。当前：

- 18 张票完成（含 #22 研究票）；
- 5 张票部分完成，缺口已逐条列明；
- 5 张票未开工，其中 3 张需要外部环境（浏览器、旧库、真实 eD2k 网络）；
- **因此 #28 尚不满足**，不能报告完成。

## 六、最终验收记录（第 59 轮）

```
命令: cmd /c test-local.cmd
结果: 100% tests passed, 0 failed out of 17
耗时: 138.91 s
```

| # | 测试 | 耗时 |
|---|---|---|
| 1–9 | core-*（store/service/http/ftp/bt 等） | 见日志 |
| 10 | win32-native-bridge | 0.01 s |
| 11 | core-credentials | 0.04 s |
| 12 | core-rate-limit | 2.02 s |
| 13 | core-kad | 2.48 s |
| 14 | core-ed2k | 16.79 s |
| 15 | core-bt | 13.79 s |
| 16 | ui-worker | 7.78 s |
| 17 | ui-window | 9.66 s |

发布包（同轮重建，11 文件 / 16.0 MiB）真实启动：

```
window: appeared after 220 ms
process: exited after 313 ms with code 0
evidence: .pi/evidence/release-final/run-.../smoke.log
```

## 七、总验收

#28 的结论单独记录在 [win32-acceptance-report.md](win32-acceptance-report.md)：**未通过**，原因与待决事项在那一页。
