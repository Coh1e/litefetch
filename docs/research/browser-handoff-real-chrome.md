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