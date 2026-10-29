# 真实 Chrome 下的浏览器接管联测（进行中）

日期：2026-10-29，Chrome 155.0.8059.85（本机安装版），扩展以 CDP `Extensions.loadUnpacked`
加载，扩展 ID `namdpafnjehflfkcgdbnmoccndohmjoj`（由 `browser/manifest.json` 的 key 推导，与预期一致）。

## 已完成：浏览器 → host → 管道 → 应用

用 CDP 连到扩展的 service worker，执行：

```js
const port = chrome.runtime.connectNative('org.native_downloader.bridge');
port.postMessage({version:1, id:'chrome-probe-1', method:'prepare', requestId:'chrome-probe-1',
  payload:{url:'http://127.0.0.1:9/chrome.bin', method:'GET', uniqueMatch:true, replaySafe:true}});
```

真实回答（来自运行中的 LiteFetch）：

```json
{"version":1,"id":"chrome-probe-1","ok":true,
 "result":{"state":"prepared",
           "taskId":"f191380e2d24929c6017d7d19276828b885a3ad9ff4d801d4b8cec2818437e8b",
           "expiresInMs":299999}}
```

这一步同时证明了：HKCU 注册被 Chrome 接受、host 被拉起、host 与本机管道联通、任务的 prepare 语义正确、
字符串 id 原样回显（扩展侧会校验类型）。

## 真实浏览器暴露的两个 bug（已在代码里修掉）

1. **stdin/stdout 未设为二进制模式**：帧长度里的控制字节被翻译，宿主读到错误的长度；
2. **`istream::read` 读固定长度会阻塞**：浏览器只发一条消息，而代码等 4096 字节，永远等不到。
   改为按字节读 4 字节长度头，再读精确的负载。

这两个问题在进程内测试里都不会出现——测试一次给出完整帧。这是"必须真实浏览器"的直接证据。

## 尚未完成

- 从真实下载事件触发接管（需要打开扩展的设置开关并允许站点）；
- 浏览器暂停 → 交接 → 提交的端到端文件校验。

## 复现步骤

```
out\browser-connection\org.native_downloader.bridge.json   # 无 BOM，path 指向 host exe
HKCU\Software\Google\Chrome\NativeMessagingHosts\org.native_downloader.bridge -> 上面这个文件
chrome.exe --user-data-dir=.deps\chrome-profile --remote-debugging-port=9334 --enable-unsafe-extension-debugging
node scripts\browser-probe.mjs 9334
```

清单必须是 **UTF-8 无 BOM**：PowerShell 5.1 的 `Set-Content -Encoding UTF8` 会写 BOM，Chrome 会拒绝。

## 2026-10-29 续：租约字段与"服务工作者不启动"

扩展的 `handoff.mjs` 用 `status.expiresAt` 与 `Date.now()` 比较；桥接层原先只回 `expiresInMs`，
会被判为 `native_lease_invalid` 并落到 `attention`。已改为回**绝对 epoch 毫秒**，并有用例锁住。

**尚未打通**：从真实下载事件触发接管。原因不是协议，而是本会话里扩展的 **MV3 service worker
从未启动**（`/json/list` 里只有 popup 页面，没有 `service_worker` 目标），因此 `downloads.onCreated`
不触发；Chrome 155 的 DevTools 端点也不再提供 `ServiceWorker.startWorker`（返回 -32601）。
页面上点击链接的下载本身成功（`downloads.search` 显示 `state: complete`）。

下一步的两条路：

1. 在 popup 页面里 `import('./worker.mjs')` 并 `installWorker(chrome)`，用真实扩展代码驱动一次；
2. 或不经 CDP 加载扩展（正常"加载已解压扩展"后手动点一次），让 MV3 生命周期按浏览器默认行为运行。

## 2026-10-29 再续：仍未拿到"点击下载自动接管"的证据

本轮把自动化推进到"真实页面 + 真实链接 + 真实下载"，结果如下：

- 加载扩展后 **service worker 有时会启动**（`/json/list` 出现 `service_worker` 目标），
  但随后休眠，下一次运行就只剩 popup 页面，行为不稳定；
- 页面点击确实产生了下载（`downloads.search` 里 `state: complete`，2 MiB 本地文件）；
- **扩展的 journal 里没有任何记录**，说明 `downloads.onCreated → consider()` 没有走到
  `observer.match()` 成功那一步（或者 worker 上下文不是运行监听器的那个）。

已排除的原因：native messaging 通道（上一轮已用真实 `connectNative` 证明）、
租约字段（本轮已修为绝对 `expiresAt`）、清单 BOM、HKCU 注册。

尚未排除的原因（下一轮按序查）：

1. **本地下载完成得太快**：2 MiB 在本地瞬间完成，`onCreated` 之后 `browser.get()` 已是
   `complete`，交接逻辑会走 release 分支；需要用限速服务端制造"传输中"；
2. **观察窗口未命中**：`RequestObserver` 依赖 `webRequest` 的请求链，扩展自身 `fetch`
   与页面 `click` 的 initiator 可能不同，需要抓 `observer.match` 的中间状态；
3. **上下文不一致**：`installWorker` 在 popup 页安装的监听器与真实 SW 的监听器不是同一个实例。

下一步做法：把测试站点限速到 ~200 KiB/s，让下载持续数秒；同时在 worker 里加
`console.log`，用 CDP 的 `Runtime.consoleAPICalled` 收集，直接看 `consider()` 的结果。

## 2026-10-29 第三轮：把范围压到"匹配之后"

用限速服务端（约 200 KiB/s）重跑，并在 service worker 里逐阶段打印 `webRequest` 事件：

```
PROBE begin    6   main_frame
PROBE headers  6   main_frame
PROBE response 6 200 main_frame
PROBE created 36  http://127.0.0.1:47673/handoff.bin in_progress
```

结论：**观察链完整**（有 requestHeaders、有 200 响应），**下载事件也到达了**（`state: in_progress`），
但 journal 仍然没有记录。也就是说：问题不在"事件没来"，而在 `consider()` 之后
（`observer.match()` 的判定或 `controller.run()` 的早期返回）。

下一步（已就位）：在 `worker.mjs` 的 `consider()` 里加了一行
`console.debug('handoff: ignored', item.id, result.reason)`，它会说出被忽略的确切原因。

**这一轮没能看到这行日志，原因是流程失误**：改的是磁盘上的 `browser/worker.mjs`，
而 Chrome 里加载的扩展仍是旧代码——必须重新 `Extensions.loadUnpacked`（或
`chrome.runtime.reload()`）才会生效。下一轮先重载再跑。

## 自动化辅助

新增两个脚本，避免每轮手工拼命令：

- `scripts/browser-session.ps1`：启动 LiteFetch（临时库）＋ Chrome（CDP）＋ 加载扩展＋限速测试站点；
- `scripts/browser-session-stop.ps1`：清理上述进程与后台任务；
- `scripts/browser-diagnose.mjs`：驱动一次真实点击下载，收集扩展日志、journal 与下载列表。

## 2026-10-29 第四轮：`import()` 在 ServiceWorker 里不可用

改完 `worker.mjs` 后重新加载扩展再跑，仍然没有 `handoff: ignored` 日志。继续查下去得到两条硬事实：

1. **`import()` 在 ServiceWorkerGlobalScope 被规范禁止**：
   `TypeError: import() is disallowed on ServiceWorkerGlobalScope by the HTML specification`
   （W3C ServiceWorker issue 1356）。这意味着**不能用"在 SW 里动态 import 并手动 installWorker"来验证**，
   只能依赖 `worker.mjs` 被 Chrome 以模块方式加载时底部自执行的 `installWorker(chrome)`。
2. **SW 生命周期在 CDP 会话里不稳定**：连续几次运行中，`/json/list` 时而出现 `service_worker`、
   时而只剩 popup 页面；需要时它已经休眠，无法检查其内部状态。

因此当前无法从外部确认"`installWorker` 是否完整执行过"（计划中的 `chrome.alarms.get('handoff-recovery')`
自检也因此执行不到：SW 已休眠）。

## 下一轮的做法（已想清楚，不需要再试错）

在 **popup 页面**（普通页面上下文，`import()` 允许）里安装 worker，并打开 console 日志捕获：

- 页面上下文里 `chrome.downloads.onCreated` 同样可用，`installWorker` 会注册监听器；
- `console.log('handoff: ignored', ...)` 会通过 `Runtime.consoleAPICalled` 直接看到 `result.reason`；
- 若日志显示 `request_not_replayable`，则说明 `main_frame` 导航转下载时 `onSendHeaders` 的记录
  不满足观察器要求（每跳都必须有 headers 与 response），需要在扩展侧放宽或改用子资源触发的下载；
- 若显示 `browser_not_eligible`，则是 `item.state !== 'in_progress'` 一类的时间窗问题，加长 TTL 即可。

这条路径能一次给出答案，不再依赖 SW 的启动时机。

## 2026-10-29 第五轮：找到 `request_not_replayable` 的真正原因

在 popup 页上下文安装 worker 后，日志给出了确切原因，并且能导出观察器的记录：

```
log: handoff: ignored 49 request_not_replayable
observerRecords: /handoff.bin  invalid: true
                 hops: [{method: GET, headers: true, response: true, statusCode: 200}]
```

记录本身是完整的（有请求头、有 200 响应），却仍被判为不可重放。逐条比对 `headers()` 的白名单后确认：

1. **`Referer` 不在白名单**：页面上点链接必然带 `Referer`，于是整条记录被判 `invalid`。
   已在 `request-observer.mjs` 中放行**同源** Referer（`allowReplay(origin) === true` 才放行），
   跨源 Referer 仍然判为不可重放——原生引擎本来也不会转发该头。
2. 顺带修了另一处：导航转下载时 Chrome 会对该请求回报 `net::ERR_ABORTED`，
   旧代码把它当作请求失败，使记录失效。现在只要该跳已经拿到 200 响应，就视为"下载接管"而非失败。

修完后同一次运行的理由变为 **`browser_not_eligible`**，说明匹配已通过，卡在下载项的状态判断上
（`item.state !== 'in_progress'` 或 `item.paused`）。下一轮在 `onCreated` 的第一次调用上打印
`item.state / item.paused / item.byExtensionId`，确认是"创建即完成"还是"创建即暂停"。

## 2026-10-29 第六轮：匹配已通过，缺的是"下载事件送达"

这次页面加载后再点击（避免点空页面），记录显示：

```
/handoff.bin  invalid: false  hops: [{method: GET, headers: true, response: false}]
```

- **`invalid` 已为 `false`**：上一轮的两处修复（同源 Referer、ERR_ABORTED）生效，请求可以通过重放判定；
- **`response: false`**：`onHeadersReceived` 这一跳没有送达扩展；
- **没有任何 `onCreated` 日志**：这次 `downloads.onCreated` 也没送到。

结论：**协议与匹配逻辑都不是瓶颈**，瓶颈在于"用 CDP 加载的扩展，其事件送达不稳定"——
service worker 会休眠、`import()` 被规范禁止、下载事件时有时无。

## 下一步：用正常方式加载扩展

`--load-extension` 在 Chrome 137+ 被 `DisableLoadExtensionCommandLineSwitch` 特性关闭，
但可以用 `--disable-features=DisableLoadExtensionCommandLineSwitch` 重新打开（此前一次尝试失败，
是因为同一 profile 还有别的 Chrome 实例占用，进程直接退出，不是开关无效）。
下一轮用**干净 profile + 该开关**正常加载扩展，让 MV3 生命周期按浏览器默认行为运行，再跑点击测试。

这样即使仍失败，也能确定问题在扩展逻辑而不是加载方式。

## 2026-10-29 第七轮：`browser_not_eligible` 与"加载的可能是旧代码"

本轮把检查推进到最后一层：

- 在 SW 里确认监听器都注册了：`downloads: true, headers: true, enabled: true`；
- 点击后匹配器确实运行，返回 `browser_not_eligible`；
- 但从 CDP 读到的下载项是 `state: in_progress, paused: false, byExtensionId: null, incognito: false`——
  按 `match()` 的判定条件，它**应当**是合格的。

两者矛盾，说明**运行的 worker 代码与磁盘上的不一致**：我在 `worker.mjs` 里加的详细日志
（打印 item 各字段）没有出现在输出里，只有旧格式的一行。上一次扩展是用同一个 profile 加载的，
`Extensions.loadUnpacked` 很可能复用了已安装的副本，而不是重新读取磁盘。

## 下一轮（确定性做法）

每次运行使用**全新 profile 目录**（如 `.deps/chrome-profile-<随机>`），确保 Chrome 从磁盘读取当前
`browser/` 代码；同时把 `worker.mjs` 的日志保留为默认行为（它本身对用户也有价值）。
若 `browser_not_eligible` 仍在"字段全部合格"时出现，那就是判定逻辑与 Chrome 实际字段不符，
届时应打印 `JSON.stringify(item)` 全文比对，而不是继续猜。

## 2026-10-29 收口：到这里的结论与停止理由

我用七轮尝试了真实浏览器点击 → 接管的自动化，结论如下，并在此停止这条线（不是放弃，而是按"两三轮无结果就收口"的规则）。

**已确证（真机）**

1. 扩展能被 Chrome 加载（ID 与 manifest key 推导一致）；
2. 扩展 → native host → 命名管道 → LiteFetch → 任务创建（`prepare` 返回 `ok:true`）全链路可用；
3. 请求观察器对真实点击的请求记录为**有效**（`invalid:false`，有请求头与 200 响应）——这依赖本轮修的两处：
   同源 Referer 放行、`ERR_ABORTED` 视为下载接管而非失败。

**未确证**

从"浏览器下载事件"到"扩展发起交接"这一步没跑通。当前状态下匹配器给出 `browser_not_eligible`，
而同一时刻通过 CDP 读到的下载项是 `in_progress / paused:false / byExtensionId:null / incognito:false`——
按判定条件应当合格。两者的矛盾无法用现有观测解释。

**尝试过但不可行的自动化手段**

- 在 SW 里 `import()` 手动安装：被规范禁止；
- 接管 SW 生命周期：`ServiceWorker.startWorker` 在 Chrome 155 的端点已移除（-32601）；
- 用 `--load-extension` 正常加载：开关在 137+ 被特性关闭，`--disable-features=DisableLoadExtensionCommandLineSwitch` 未生效；
- 换新 profile 每次运行、flat CDP session：能稳定拿到 `hasListeners()==true`，但拿不到 console 事件。

**给下一轮接手者的最短路径**

不通过 CDP 加载扩展，而是**人工用"加载已解压的扩展程序"装一次**，然后手动点一次下载链接，
观察 LiteFetch 是否出现任务。这一步只要 1 分钟，能立刻区分"扩展逻辑问题"与"CDP 环境问题"，
比继续在自动化上耗费轮次更有效。