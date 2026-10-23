# 现代浏览器自动接管：官方 API 可行性与实现建议

核查日期：2026-10-18。只读官方文档；未编写/安装扩展，未修改注册表或浏览器。以下“建议”是工程设计，尚未通过浏览器实测。

## 结论

Windows 上可用 **Chrome/Edge MV3 扩展 + Native Messaging 小型 C++ host + Qt 应用本地 IPC** 实现自动接管常规 HTTP(S) 文件下载。Firefox 使用 WebExtension 共用业务逻辑，但单独生成 manifest。必须明确：这是对可重放下载的接管，不是无损接管任意浏览器请求，也不保证浏览器完全不接收初始字节。

Chrome `downloads.onCreated` 在下载开始时通知；DownloadItem 提供原始 URL、重定向后 finalUrl、referrer、文件名、MIME、状态等，但没有原请求 method/body。`cancel()` 取消任务；`erase()` 仅删除下载历史，不删除文件。需要 `downloads` 权限。[Chrome downloads](https://developer.chrome.com/docs/extensions/reference/api/downloads)

## 事件与兼容边界

| 项目 | 官方事实 | 实现建议 |
|---|---|---|
| `onCreated` | Chrome/Firefox 都提供创建下载事件 | 作为统一接管入口；先检查自动接管开关、URL scheme、站点/扩展名规则、本机可用状态 |
| `onChanged` | 文件名、状态等信息可以随后变化 | 更新元数据；不能用“创建时未知大小”直接判断为不支持 |
| `onDeterminingFilename` | Chrome 用于覆盖文件名；每监听器必须调用 suggest 一次，异步需 return true；等待它只延迟完成 | 可用来补文件名，不能当作外部下载 API、网络暂停点或无限等待本机的锁 |
| Firefox 差异 | Mozilla 下载 API 列表/兼容数据未提供该命名事件 | 不依赖 onDeterminingFilename；用 onCreated/onChanged。启动时做 API feature detection |

来源：[Chrome downloads events](https://developer.chrome.com/docs/extensions/reference/api/downloads#event-onDeterminingFilename)、[Mozilla onCreated](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/downloads/onCreated)、[Mozilla downloads](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/downloads)、[Mozilla 官方兼容数据](https://github.com/mdn/browser-compat-data/blob/main/webextensions/api/downloads.json)。

Chrome 的 MV3 worker 可能空闲约 30 秒后终止，内存全局变量不能作为可靠事务状态；connectNative 的有效连接可保持 worker 活跃，但仍需处理断开和崩溃。接管事务、回退标记、幂等键放 extension storage；敏感请求体/Cookie 不存同步存储。[worker 生命周期](https://developer.chrome.com/docs/extensions/develop/concepts/service-workers/lifecycle)

事件监听器应在脚本顶层同步注册，不能等待读取设置之后才注册。[事件注册](https://developer.chrome.com/docs/extensions/develop/concepts/service-workers/events)

Firefox 当前官方文档仍将 `background.service_worker` 标为不支持，MV3 使用 `background.scripts` 的事件页。建议分别生成 Chromium/Firefox manifest，避免把 Chrome 生命周期假设复制给 Firefox。[Mozilla background](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/manifest.json/background)

## 本机连接与安装

Native host 是独立进程，通过 stdin/stdout 交换 UTF-8 JSON，前置本机字节序的 32 位长度。Windows C++ 必须启用二进制 stdio；stdout 只写协议，日志去 stderr。Chrome 主机发回单消息上限 1 MiB，不适合搬运整个下载文件。扩展使用 `nativeMessaging`；内容脚本必须经扩展后台转发，后台校验发送方与字段。[Chrome Native Messaging](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging)

安装器按用户注册 host manifest 的绝对路径，避免默认要求管理员权限。浏览器注册位置：

| 浏览器 | Windows 注册键（host 名称后缀） | 允许扩展字段 |
|---|---|---|
| Chrome | `HKCU\Software\Google\Chrome\NativeMessagingHosts\<host>` | `allowed_origins`，固定 extension origin，无通配符 |
| Edge | `HKCU\Software\Microsoft\Edge\NativeMessagingHosts\<host>` | `allowed_origins`；Chrome/Edge 商店 ID 都加入 |
| Firefox | `HKCU\Software\Mozilla\NativeMessagingHosts\<host>` | `allowed_extensions`，显式 Gecko addon ID |

来源：[Chrome host 注册](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging#native-messaging-host-location)、[Edge Native Messaging](https://learn.microsoft.com/en-us/microsoft-edge/extensions/developer-guide/native-messaging)、[Mozilla Native Messaging](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_messaging)。

建议 host 只做协议校验、启动应用和带用户隔离的本地 IPC；下载由应用处理。host 不接受 shell 命令，也不把网页提供的任意路径当最终保存路径。可执行文件与 manifest 由安装器部署，扩展由用户安装并授权；便携包需提供显式“启用浏览器集成”操作。当前研究没有执行这些注册。

## 身份与请求重放

### Cookie 与 Referer

`cookies` API 需要 cookies 权限和对应 host 权限；支持 storeId、分区 Cookie 的 partitionKey，不能只用裸域名读取整库。Firefox DownloadItem 有 cookieStoreId，需保留容器上下文。[Chrome cookies](https://developer.chrome.com/docs/extensions/reference/api/cookies)、[Firefox DownloadItem](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/downloads/DownloadItem)

建议无登录直链接管只申请 downloads/nativeMessaging/storage；开启“接管需登录的网站”时再申请 cookies、webRequest 和相关站点权限。使用实际观察到的请求上下文，凭据仅传给对应任务；本机不得把 Cookie/Authorization/Referer 无条件带到跨域重定向。若无法确定分区、容器或身份完整性，保留浏览器下载。

### 请求观察不等于任意重放

Chrome MV3 普通商店扩展仍可观察 webRequest，但大多数扩展不能用 webRequestBlocking；请求体须显式请求 requestBody；Cookie/Referer 等头需 extraHeaders 才可观察，且 API 并不提供完整最终线上请求头。需要相应 host/initiator 权限。[Chrome webRequest](https://developer.chrome.com/docs/extensions/reference/api/webRequest)

推论：仅 downloads 事件无法判定原请求必为 GET；需要有界缓存 webRequest method/URL/时间/重定向信息，匹配必须唯一，不能按同一 URL 最近一次请求盲猜。DownloadItem 没有可直接关联的 webRequest requestId，重复并发 URL 与重定向是需要实测的匹配缺口。

### POST、blob 和特殊身份

- **POST**：首版自动接管只接受已可靠识别的 GET。POST 可能创建导出任务、消费一次性 token，取消后再次提交未必安全；即使观察到 body，也不能默认自动重放。未来仅为经测试的站点适配明确支持。
- **blob/data**：blob URL 引用浏览器内存资源并受创建上下文/分区限制，不能把该字符串交给 libcurl 当公网地址下载。默认留浏览器；未来可在浏览器完成后导入本地文件，不能承诺外部断点续传。[Mozilla blob URL](https://developer.mozilla.org/en-US/docs/Web/URI/Reference/Schemes/blob)
- **浏览器专用身份**：客户端证书、集成认证、页面脚本签名、防机器人挑战、一次性/即将过期 URL 列入保守回退。此项是基于外部客户端请求环境不同的工程判断，需要站点实测。
- **magnet/ed2k**：它们不一定产生浏览器 downloads 事件。采用显式 Windows 协议关联及扩展菜单/链接操作作为独立入口；协议关联需要应用安装流程处理，不能用 HTTP 自动接管事件替代。

## 推荐接管事务与回退

以下是建议状态机，不是浏览器提供的原子转移 API：

1. `observed`：匹配规则与请求上下文；不支持或有歧义，立即让浏览器继续。
2. `browserPaused`：尝试 pause；失败、已完成或用户另有操作，则放弃接管。此时可能已有浏览器临时字节，不能声明零重复请求。
3. `prepared`：host 将任务及幂等键持久化为“等待交接”，**不启动网络**；回复 taskId、过期时间与 accepted/rejected。
4. 收到准备确认后取消原浏览器任务；检查取消后的实际状态。若已完成，abort 本机任务；若确认取消，再发送 `commit`，本机开始下载。
5. 取消之前任一步失败，abort 本机预备任务并 resume 浏览器。应用的 prepare 租约到期自动作废，防止失联后重复开始。
6. 取消成功但 commit 确认丢失，先用 taskId 查询本机事务，不能马上重建浏览器请求。确认本机未启动且 GET 可重放后，提供“交回浏览器”；否则提示用户重新点击原网站。

取消后的浏览器任务不能被假定为可 resume；自动回退可能需要创建新下载，它也不是对 POST/一次性地址的无损恢复。建议取消前尽可能保证 host 已持久化、IPC 可用和目标目录可写；不要为“验证可下载”预先消费一次性地址。

浏览器 fallback 新建下载前持久化单次绕过标记，并使用本扩展的 byExtensionId、downloadId、URL、时间窗共同识别；自发下载忽略，onChanged 重入幂等。应用与扩展共用 requestId 防重复。`erase` 只用于确认交接后的界面清理，不能当取消或删文件操作。不要默认隐藏全部浏览器下载界面，避免回退后用户找不到任务。

## 落地顺序与验收

推荐先 Chrome/Edge：直链 GET＋重定向＋Content-Disposition 文件名＋本机离线自动保留浏览器＋暂停/取消失败回退＋扩展 worker 重启幂等。随后增加 Cookie/Referer 与 Firefox。POST/blob 明示留浏览器，不能以“捕获到了 URL”计为接管成功。

必须覆盖：同 URL 并发、未知大小、小文件瞬间完成、原应用未运行、host 崩溃、prepare 后断线、cancel 后断线、worker 重启、用户同时取消、登录过期、跨域重定向不泄漏凭据、Cookie 分区/Firefox 容器、文件名非法字符、其它下载扩展冲突。真实浏览器与本地可控 HTTP 测试服务的结果，才是最终能力证明。

与旧版区别：旧包包含 IE BHO 及历史 Chrome/Firefox 扩展，不能推导其在现代浏览器仍可运行。新版采用公开扩展 API 和显式本机 host，界面保持 Qt 原生；没有接入迅雷专有服务的必要。
