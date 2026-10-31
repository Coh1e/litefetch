# 浏览器集成：接管事务核心

> 本文记录 2026-10 的浏览器接管状态。部分路径和“Qt”描述已经过期：Qt 已从产品源码移除，
> 测试位于 `browser/*.test.mjs`。当前结论以 [最终交代](../docs/specs/final-report.md)
> 和 [浏览器联调](../docs/research/browser-handoff-real-chrome.md) 为准。

此目录目前包含Chrome/Edge MV3开发扩展的manifest、service worker、站点设置弹窗、事务逻辑和API适配层。C++ host、BrowserBridge与SQLite任务事务已接通并完成实际HTTP整链测试；浏览器host注册及真实浏览器加载/接管尚未验收。

扩展默认关闭，仅在开关启用且请求链所有来源均明确列入允许站点时考虑接管。开发弹窗会说明尚未接通本机下载器。关闭设置会阻止尚未取消浏览器的在途准备事务继续接管；已经取消浏览器的事务仍需恢复查询，不能直接遗弃。

`handoff.mjs` 通过适配接口协调：暂停浏览器→本机持久准备→确认取消→本机提交。每次有副作用前记录状态；同一浏览器任务的并发事件合并，同一 requestId 用于本机去重。扩展持久记录只含事务标识、阶段和租约，不保存下载网址、Cookie 或请求头。传给 prepare 的请求仍可能包含敏感网址，未来 Native host 必须保护其本机存储。

本轮未改Qt应用的下载行为，没有安装浏览器扩展或注册Native host。package-browser-extension.ps1把9个运行文件按明确清单复制并压缩到out/package，排除测试、日志和任务数据；目前只做开发包，不是发布完成品。

开发manifest的key已固定，预计ID为namdpafnjehflfkcgdbnmoccndohmjoj（真实浏览器仍需核对）。scripts/prepare-browser-connection.mjs生成项目内host清单、运行配置预览和注册计划，默认不复制到host旁、不执行HKCU注册。四份文件位于out/package/browser-connection；注册前必须检查既有值并取得对项目外配置的明确授权。原生设置界面尚未提供，因此当前仍是开发接线。

后续更新：Qt工具栏“浏览器接管”已提供本机开关/目录设置，保存后生效并可重启加载，关闭仍保留在途事务。它尚不负责host配置复制或浏览器组件注册，真实浏览器接管仍需安装与联测；详见analysis/notes/downloader-browser-native-settings.md。

连接文件准备更新：原生设置里的“准备连接文件”已能导出与当前GUI设置一致的运行配置与host清单，并拒绝覆盖另一套配置。它仍不修改注册表或安装扩展；Chrome临时注册与真实浏览器联测等待明确授权。详见analysis/notes/downloader-browser-prepare-ui.md。

## 适配接口约束

- `journal.get(downloadId)` / `set(downloadId, record)`：按浏览器配置文件隔离，写成功后才返回；原子替换，存储失败必须抛异常。adapters.mjs 的 storageJournal 已适配 Promise 形式的 storage API，按事务独立键写入，提供 list 供恢复使用。浏览器自身的断电持久性尚未验证。
- `browser.get/pause/resume/cancel`：返回实际下载状态；get 无匹配时返回 undefined。每项必须有明确超时策略。cancel 成功的 Promise 不足以证明取消，需要重新读取 state/error。
- `native.prepare(requestId, request)`：返回 `{state:'prepared',taskId,expiresAt}`；必须先持久化租约和请求，**不得启动网络**。相同 requestId 绑定同一请求，不能被不同 payload 覆盖；过期且未提交的准备记录必须自动作废。
- `native.query(requestId)`：权威查询；prepared 状态含 taskId/租约，committed 状态含 taskId。提交成功但响应丢失时，必须能查到 committed。
- `native.commit(requestId)`：幂等提交，仅允许未过期的 prepared 任务；把任务变成可调度与标记事务已提交必须具备崩溃一致性。最终需与 Qt 任务队列整合，不能“先开始下载，之后补记状态”。
- `native.abort(requestId)`：幂等作废尚未提交的准备任务，不能删除或停止已提交任务。

原生端租约及原子提交已由TaskStore实现。adapters.mjs的JS测试由浏览器API/Port替身驱动；C++整链测试另外实际启动Native host，对接生产BrowserBridge类、真实SQLite队列和受控HTTP服务器，验证最终文件。两类证据仍不可合并成真实浏览器接管证明；断电、跨用户ACL和敏感数据保护尚未完成验收。

## 浏览器API和本机消息适配

downloadsAdapter 调用 downloads.search/pause/resume/cancel，按ID重新核对状态，并给浏览器调用设置超时。超时不等于浏览器取消底层动作，协调器会保留不确定性。

NativeClient 使用 runtime.connectNative，按独立消息ID匹配响应，支持并发乱序响应。断线、超时或协议错误会拒绝所有在途调用并移除监听器；下次操作重新连接。每条JSON消息限制64KiB，同时最多32条在途请求。未收到 commit 确认不会直接认定失败，也不会自动重建浏览器请求。

消息协议：请求 `{version:1,id,method,requestId,payload?}`；响应 `{version:1,id,ok:true,result}` 或 `{version:1,id,ok:false}`。method仅由客户端固定方法发出：prepare/query/commit/abort。requestId标识持久事务，id仅关联一次消息往返。host需要同目录native-host.json明确允许的扩展origin和本用户IPC名，配置缺失时拒绝运行，未注册或写入占位配置。GUI以显式开发选项--browser-bridge-config启用应用服务，另需同端点的配置和已有保存目录；没有最终GUI设置入口。详见analysis/notes/downloader-browser-bridge-integration.md。

API依据：[Chrome downloads](https://developer.chrome.com/docs/extensions/reference/api/downloads)、[storage](https://developer.chrome.com/docs/extensions/reference/api/storage)、[Native Messaging runtime接口](https://developer.chrome.com/docs/extensions/reference/api/runtime#method-connectNative)，2026-10-20核对。适配器采用Promise API，旧浏览器兼容性未验收。

请求入口仅接受观察层已确认唯一匹配、可重放的 HTTP(S) GET。`uniqueMatch/replaySafe` 必须由可信扩展后台观察层计算，不能接受网页内容直接设置。request-observer.mjs已实现有界观察和候选关联，worker.mjs已完成API事件注册代码；尚未在真实浏览器中加载验收，不能把这些布尔字段视作已解决所有请求关联、登录上下文或一次性网址问题。

## 请求观察与关联

RequestObserver 接收 begin/headers/response/redirect/error，再用 DownloadItem 的原始URL、finalUrl与开始时间匹配同一请求链。默认保留30秒、最多256个请求、每链最多10跳；溢出会暂停整个时间窗的匹配，避免丢失竞争请求后误判为唯一。新worker没有历史时直接放弃关联，不猜测最近请求。

不按文件后缀判断，因此无扩展名附件也可成为候选。同URL并发即使其中一个是POST，也会判为歧义。重复认领只允许同一个downloadId。观察不完整、认证挑战、206部分响应、Cookie/Referer/Range/认证/自定义请求头、私密浏览、其他扩展创建的下载及非GET都留浏览器。header值不会保存在观察缓存中；URL只在有界内存中保留。

每个重定向来源都必须通过显式 allowReplay 策略，默认全部关闭。popup.html/popup.mjs已提供开关和允许站点输入；暂不能据此声称端到端自动接管已可用。允许策略是对已知可重放来源的工程配置，并不能从GET方法证明链接可重复使用。

worker已注册onBeforeRequest、带requestHeaders/extraHeaders的onSendHeaders、响应头与重定向/失败事件；监听器在读取设置之前同步注册。开发manifest申请全部HTTP(S)来源观察权限，以覆盖请求目标与initiator；站点允许策略额外限制实际接管。尚未验证真实权限/事件覆盖，后续分发前仍需核对权限体验。webRequest不保证暴露所有线上请求头；即使当前可见头里没有Authorization，也不能证明浏览器没有使用认证。DownloadItem没有与webRequest直接关联的requestId，因此此处是可见候选的保守关联，仍需真实浏览器对并发、权限遗漏、worker重启和跨事件时序做端到端验证。[Chrome webRequest官方边界](https://developer.chrome.com/docs/extensions/reference/api/webRequest)，2026-10-20核对。

worker在启动、安装、每次自身求值和一分钟恢复alarm中检查既有事务，避免只依赖onStartup。暂停前及取消前会复核下载的原始/最终URL。请求事件、下载事件和Native Messaging接线已由API替身集成测试覆盖；没有把它当真实Chrome/Edge验收。Firefox manifest与事件页适配仍待实现。

## 恢复与边界

- 取消前失联：作废本机准备，并尽可能继续由本模块暂停的浏览器任务。
- 已确认取消后失联：保留 cancelled/committing，恢复时查本机状态；已提交则确认，仍 prepared 则按同一 requestId 提交。
- 崩溃发生在浏览器动作调用与写入确认之间：标 attention，不猜测取消由用户还是扩展发起。后续需要原生/扩展界面提供明确恢复动作。
- 已提交、已释放和 attention 为当前核心的终态；人工恢复/清理接口尚未实现。
- 本机状态不明确时不自动创建替代浏览器请求，避免重复下载和消耗一次性链接。
- POST、blob/data、用户已暂停任务及不可靠请求关联留给浏览器。对这些条件的真实浏览器识别仍未实现。

## 验证

在项目根目录，使用现有 Node 运行：

```text
node --test browser/handoff.test.mjs browser/adapters.test.mjs browser/request-observer.test.mjs browser/worker.test.mjs
```

测试涵盖确认丢失、事件重复、用户取消、瞬间完成、存储失败、进程状态重建和实际文件 journal 重开。文件 journal 是测试适配，不是生产实现，也没有验证断电持久性。所有测试数据仅在 downloader/out/verification 中创建并清理。

Chrome for Testing 155 已通过真实扩展→Native Messaging→Qt/SQLite/libcurl 联测：公开 GET 自动接管、禁用与离线回退、worker 重启后的丢失确认去重，以及原生进程重启后的 prepared 事务恢复；最终文件逐字节核对。运行脚本与证据边界见 [联测记录](../docs/research/downloader-browser-real-chrome.md)。测试临时注册 Chrome HKCU 项须有明确授权，并在 finally 恢复；当前不提供永久注册入口。正式版个人 profile 安装、Edge/Firefox、登录上下文、整个浏览器进程崩溃与真人处理 attention 状态仍未验收。
