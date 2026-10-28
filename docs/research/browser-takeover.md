# 浏览器接管方案与平台边界（#22）

状态：研究 + 最小验证。**不代表产品功能完成**（对应 Issue #22 的验收口径）。
日期：2026-10-28。

## 1. 结论摘要

1. Chrome、Edge、Firefox 都只提供一条稳定的"浏览器进程 → 本机程序"通道：**native messaging**（浏览器拉起宿主进程，stdin/stdout 上跑 32 位小端长度 + UTF-8 JSON 的帧）。没有第二条受支持的 IPC 通道。
2. 接管一个下载需要两步，缺一不可：
   - 扩展在 `downloads.onCreated` 里**取消**浏览器自己的下载；
   - 扩展把 URL 经 native messaging 交给本机程序。
   只做第二步，浏览器仍会下载一份，去重和"只有可靠接收后才改变原下载"都无从谈起。
3. 认证上下文（Cookie/Authorization）**没有通用转交机制**。唯一可行路径是扩展在用户授予 `cookies` 权限后读取目标域的 Cookie 并随消息带过来；这既需要用户授权，也会把敏感数据送进本机进程，必须默认关闭并显式提示。
4. 因此产品边界应当写成："能读 Cookie 且用户允许时接管；否则保留浏览器下载并提示"，不能承诺"全站接管"。

## 2. 平台事实与来源

| 事实 | 来源 | 可验证性 |
|---|---|---|
| native messaging 帧 = 4 字节小端长度 + JSON，浏览器侧上限 1 MiB | Chrome 文档 *Native messaging*（`developer.chrome.com/docs/extensions/develop/concepts/native-messaging`）、MDN *Native manifests* | 本票已用模拟对端验证帧格式与上限 |
| 宿主清单需声明 `allowed_origins`（Chromium）/`allowed_extensions`（Firefox），路径与注册位置随浏览器不同 | MDN *Native manifests*；Chromium 文档同上 | 未验证（需注册，见 §5） |
| 扩展可取消自己的下载：`chrome.downloads.cancel(id)` | Chrome 文档 *chrome.downloads* | 未验证（需真实扩展） |
| Firefox 使用同一套 WebExtensions `downloads` API，但宿主清单字段不同 | MDN *Native manifests* | 未验证 |
| 浏览器不提供把会话凭据交给外部程序的机制；扩展可用 `cookies` API 读取 Cookie（需权限） | Chrome/MDN `cookies` API 文档 | 未验证（需真实扩展 + 用户授权） |

> 说明：本环境无法访问 `developer.chrome.com` 与 `developer.mozilla.org`（DNS 解析到非公网地址被拒绝），以上条目依据我已知的稳定文档内容；可检索到的第三方整理见 [browser-native-messaging 笔记](https://github.com/sheeki03/tirith/blob/18b52d115c978877bc3b66fc55bfcabae52913a7/docs/browser-native-messaging.md) 与 [CDM 扩展实现](https://github.com/adel-bakhshi/cdm-browser-extension#1)。正式落地前应在可联网环境复核一次原文。

## 3. 参考产品（IDM）公开行为

以下来自公开说明与本机可观察行为，**不是**对其内部实现的猜测：

- 安装浏览器扩展后，扩展在页面/下载事件里把链接交给 IDM（本机进程），并取消浏览器下载；
- IDM 提供"接管/不接管"开关与站点排除列表；
- 需要登录的链接，IDM 的表现取决于浏览器是否把 Cookie 交给它；不交时通常退回浏览器下载。

对我们的意义：**接管即取消**是参考产品的共同做法；"认证不可转移时回退"同样是公开可见的行为，而不是我们的臆测。

## 4. 方案

### 4.1 触发
- 扩展监听 `downloads.onCreated`，只处理 `http/https/ftp/magnet/ed2k`；
- 用户在扩展选项里显式启用"交给 LiteFetch"，默认关闭（对应"未启用不接管"）。

### 4.2 交接
- 扩展发送 `{url, referrer, userAgent, cookies?, downloadId}`；
- 本机程序校验 scheme 白名单（已实现，见 `src/win32/native_bridge.*`），拒绝 `javascript:`、`file:` 等；
- 接受后回复 `{accepted:true, detail}`，扩展才 `downloads.cancel()`；拒绝时回复 `{accepted:false}`，扩展**保持原下载**。

### 4.3 认证
- 默认不请求 Cookie；
- 用户在扩展里显式开启后，仅对目标域读取并随消息发送，本机侧只在内存中使用，不落盘（与 #21 的凭据策略一致）；
- 读不到 Cookie 时按"未认证"处理，失败后回退浏览器下载并提示。

### 4.4 去重
- 以规范化 URL（去掉片段、统一大小写 scheme/host）为主键；
- 同一 URL 在 5 秒窗口内重复到达时，只建一个任务，其余回复 `accepted:true` 但不新建（避免扩展重复取消）；
- 已有完全相同 URL 的任务时定位到它，不新建（复用现有 `taskIdentity` 语义）。

### 4.5 回退
- 本机程序未运行/拒绝/超时（2 秒）：扩展不取消下载；
- 本机程序崩溃：扩展收到通道关闭后不取消；
- 用户关闭接管：扩展完全不发消息。

## 5. 本票已做的最小验证

- **帧格式**：4 字节小端长度 + JSON 的读写、截断帧、空帧、超过 1 MiB 的帧、关闭通道 —— 用模拟浏览器对端在 `win32-native-bridge` 测试里验证。
- **交接契约**：合法 URL 接受、scheme 白名单拒绝、缺 URL 拒绝、非 JSON 拒绝、应答结构 —— 同上。

## 6. 未验证 / 需要人类决定

1. **真实注册**：安装 native messaging 宿主清单需要写注册表（Chromium）或放清单文件（Firefox）。按本项目约定，这类系统级写入必须由人类执行或明确授权，本票未做。
2. **真实扩展**：本票没有发布扩展，因此"取消下载 → 交接 → 建任务"的完整链只在方案层面成立，没有端到端证据。
3. **Cookie 转交**：需要用户授权 `cookies` 权限并选择信任本机程序；是否默认提供该选项，请人类决定。
4. **Firefox 差异**：`allowed_extensions`、清单路径与 Chromium 不同，需要单独验证；是否首发就支持三浏览器，请人类决定。
