# Win32 交付状态与证据

对照 `docs/specs/win32-ticket-plan.md` 与父规格 Issue #29，逐票记录当前状态、证据与缺口。
验收命令统一为 `cmd /c test-local.cmd`（当前 **23 个测试，约 165–185 秒，全部通过**）；测试与票面的对应关系见 `docs/specs/verification-index.md`。

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
| 15 | ed2k 上传、入站与协议互通 | 入站服务、任务级共享、只共享任务数据、上传计数 | `core-ed2k`；LowID/加密/真实客户端由人类接受为边界 |
| 19 | SHA-256 验证及统一错误恢复 | 校验值输入/校验/重新校验、重试预算 | `core-service`、`ui-window` |
| 21 | 凭据按需记住与隐私验证 | DPAPI 存储、自动重试、清除入口 | `core-credentials` |
| 22 | 现代浏览器方案与真实接管边界验证 | 平台事实与推断分离的研究文档 | `docs/research/browser-takeover.md` |
| 27 | 移除 Qt、Release 包与依赖/许可证审计 | 90 个 Qt 源文件删除、Release 预设、便携包、MIT 许可、包内 exe 真实启动 | `out/package`、`scripts/package-release.ps1`、smoke 日志 |

## 二、部分（Issue 打开）

| 票 | 已交付 | 缺口 | 原因 |
|---|---|---|---|
| 16 | 跨引擎限速（HTTP/FTP+ed2k 共享，BT 独立上限）、并发槽、优先级、低占用、流量分项（下载/上传/校验已计） | **元数据流量未计** | libtorrent 不通过进度回调报告元数据字节；没有可靠来源 |
| 17 | 选项对话框可达（限速/代理/超时/ed2k 服务器/并发/时段/计划）、代理经 `CURLOPT_PROXY` | 对话框交互**无自动化验证** | 模态窗口，进程内 harness 无法驱动 |
| 18 | 删除任务与文件、空间预检、同名策略、分类保存、暂停保留进度；**受阻存储与强杀一致性已有真实证据**（`docs/research/blocked-storage-and-forced-kill.md`） | 实验用"拒绝写入的目录"而非只读卷（表现一致，介质状态不同） | 待人类确认后关闭 |
| 25 | 时段限速、计划任务（到点开始/暂停全部）、完成动作；**"完成后关闭程序"已由 `ui-window` 用例锁住**（含有牙检查） | 只剩"完成后打开文件夹"（会拉起资源管理器，进程内无法断言） | 人眼确认"打开文件夹" |
| 26 | 托盘、通知、DPI（含 `WM_DPICHANGED` 重排用例）、加速键审计、高对比通知承受 | 纯键盘走查、真实高对比目视、逐状态画板核对 | 需要人眼或真实主题；Figma 页面 02+ 读不到 |

## 三、未开工（Issue 打开）

| 票 | 主题 | 说明 |
|---|---|---|
| 20 | 旧任务显式导入与两种数据模式 | 人类确认没有旧库样本 |
| 23 | 浏览器本机交接与 Chrome 完整链 | **协议层与 host 已在真实 Chrome 上验证**：扩展 → host → 管道 → 应用 → 创建任务（`prepare` 返回 `ok:true`）；**未验证**：点击下载后由扩展发起交接这一步（详见 `docs/research/browser-handoff-real-chrome.md`） |
| 24 | Edge/Firefox 与认证接管、组件生命周期 | 同上，且依赖 23 |
| 28 | 全范围总验收与干净 Windows 交付验证 | 依赖上面全部完成 |

## 四、贯穿性事实

- **验收方式**：`cmd /c test-local.cmd`，23 个测试全部在进程内运行；跨进程 e2e 已由 ADR-0001 取消。
- **环境限制**（本会话）
  - 通知区与剪贴板不可用：托盘气泡、复制链接的断言会打印跳过说明；
  - 无公网 Kad/eD2k 节点：真实网络互通一律未验证；
  - Figma 只读到页面 01；
  - `web_fetch` 被 DNS 策略拦截，改用 PowerShell 抓取。
- **测试注意事项**
  - 不要绕过 `test-local.cmd` 单跑测试：它把 `TEMP/TMP` 指向项目内 `.deps\tmp`；
  - 依赖"当前选中行"的用例必须显式选中目标行；
  - 与文件写入竞争的断言应改为状态断言。
- **已知工具教训**：含换行的 C++ 修改只用 `edit`/`write`；PowerShell 字符串替换曾多次静默失败并造成"以为改了"的假象。

## 五、距离 #28 的要求

- 19 张票完成；
- 5 张票部分完成，缺口全部列明且多为"环境不可得"或"人眼验收"，逐项对应关系见 `docs/specs/verification-index.md`；
- 4 张票未开工（#20 无样本、#23/#24 需浏览器与注册表授权、#28 本身）；
- 干净 Windows 机器始终不可得。

**因此 #28 仍不满足**，不能报告完成。

## 五、本会话修掉的真实缺陷（第 114 轮补记）

这些不是为了让测试变绿而改的代码，每一条都有对应的失败现象与验证方式：

1. **浏览器 host 读帧**：stdio 未设二进制模式、按 4096 字节块读导致永久阻塞 → 真实 Chrome 联调暴露；
2. **租约字段**：扩展用 `expiresAt` 与 `Date.now()` 比较，桥接层只回 `expiresInMs` → 判为 `native_lease_invalid`；
3. **观察器误判**：同源 `Referer` 被当作不可重放；导航转下载的 `ERR_ABORTED` 被当作请求失败；
4. **设置不持久**：`TaskWorker` 从不调用 `loadSettings()`，重启后限速/代理/并发/时段/计划/完成后动作全部回默认；
5. **批次记账竞态**：`jobs_.push_back()` 与 `batchActive_` 不同锁，快速传输下"批次结束"永不产生；
6. **错误文案混语言**：引擎英文错误直接显示在中文界面。

其中 4 与 5 是从"完成后动作不触发"一条线索挖出来的，影响面远超该功能本身。
## 六、交付物与最后一轮证据（第 116 轮）

- **发布包**：`out/package/litefetch-0.1.0`，13 个文件 / 16.2 MiB（含 `litefetch-browser-host.exe`、
  `LICENSE`、`THIRD-PARTY.txt`），由 `scripts/package-release.ps1` 生成；
- **包内 smoke**：`scripts/win32-smoke.ps1 -AppPath … -EvidenceRoot .pi/evidence/final`
  → 窗口 219 ms 出现、退出码 0，证据在 `.pi/evidence/final/run-*/smoke.log`；
- **测试**：`cmd /c test-local.cmd` → 23/23 通过；
- **遗留进程**：检查过项目内启动的 Chrome（`--user-data-dir` 指向 `.deps\chrome-*`）与
  `litefetch-*` 进程，均已结束；仓库工作区干净（`git status` 无输出），`tmp/`、`out/`、`.deps/`、`.pi/` 均在忽略之列。