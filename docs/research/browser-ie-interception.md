# IE BHO：HTTP 识别与下载接管的函数级证据

日期：2026-10-18。输入：work/minithunder-unpacked/bho/XunleiBHO.dll；哈希、基址和语言见 analysis/exports/browser-ie-ghidra/metadata.json。
本轮是静态反编译与指令/引用检查，没有加载 BHO、修改浏览器、注入进程或运行样本。

## 不同浏览器不是同一种接管机制

Chrome 包内 JavaScript 的已证实入口是点击接管；IE DLL 则另有收发函数替换、HTTP 请求/响应识别和 socket 转交路径。不能把 Chrome 脚本的局限推广为“迅雷只有按链接后缀接管”。

## 已闭合的静态链

1. `FUN_1001782c` 保存 send/recv/WSARecv/closesocket/OpenProcess 等原入口到全局指针，调用 `FUN_1001ffe6` 传入原入口指针地址与替代函数，再由 `FUN_1001ffde` 提交。失败分支恢复原函数指针。`FUN_10017a63` 是对应解除路径。
2. send 的替代函数 `FUN_1001752b` 先调用原 send，再调用 `FUN_10012415` 解析请求并维护按 socket 跟踪的数据。
3. `FUN_10012415` 检查请求行/头结束标记、方法及路径，读取 Host、Referer、Cookie；存在 POST 判别。字段存在不代表所有请求体与编码都已正确支持。
4. recv 替代函数 `FUN_100170cd` 调原 recv 后进入 `FUN_10015da9`；WSARecv 替代函数 `FUN_100171a3` 则在同步成功时整理 WSABUF 数据后调用同一解析函数。
5. `FUN_10015da9` 识别 `HTTP/`、状态码、Location 重定向、Content-Disposition/attachment/filename、Content-Type、Content-Length 和 Content-Encoding。`FUN_100113e5` 对照 MIME 字符串表。解析器建立事件和共享跟踪项，后续交接路径见 `FUN_100140d4` / `FUN_100148a8`。
6. `FUN_100140d4` 的已反编译路径调用 `FUN_1001acab`。后者在匹配到预先登记的接收方时，将 filename、cookie、refurl、url、type、encoding、origin 写入 XLSaveAs 配置，并发送窗口消息。
7. `FUN_1001acab` 明确调用 `WSADuplicateSocketW`，把接收方 PID 作为目标，写出 WSAPROTOCOL_INFO（0x274 字节）和一段随附数据到 `<配置路径>_infow`，再通知/跟踪对方进程。

因此，旧 IE 方案至少含有“交接已经建立的连接及上下文”的实现，而非一律让下载器重新 GET。返回值依赖 socket 复制与目标进程状态，不能忽略这些条件直接称为交接必定成功。

## 不能据此宣称的内容

- 尚未完成接收方主程序读取 `_infow`、重建 socket、最终接收文件的闭环。
- socket 禁止列表让部分后续 recv 返回错误，但完整成功确认、浏览器 UI 取消/恢复和所有竞态尚未动态核实。
- 这条收发路径处理可识别的 HTTP 明文；不能据此声称在当代浏览器 TLS/HTTP2/HTTP3 下直接通用。
- DsEventBeforeNavigate2 等名称属于另一个动态加载接口集。它的加载与这条 socket 处理链要分别核实。
- Ghidra 对异常序言、标准库及间接调用存在不完整类型，函数原型和部分局部变量名称不是已恢复的原始源码。

## 对新版的约束

新版采用现代扩展允许的下载事件和 Native Messaging，不复制旧 BHO 的进程内网络钩子或 socket 迁移设计。
同样重视“上下文完整”和“接收方确认”：只在本机已可靠接收任务后取消浏览器下载；无法可靠重放的方法/URL/凭据则保留浏览器。具体官方 API 限制与测试矩阵见 browser-capture-modern.md。

这不依赖迅雷查源或加速网络，两者在需求上应独立处理。
