# aria2 RPC 独立对照

2026-10-20，未替换产品引擎或修改产品依赖清单。

## 版本

实时GitHub API结果analysis/aria2-release-check.json：官方aria2/aria2未归档；最新稳定release-1.37.0，发布2023-11-15，仓库pushed_at为2026-06-25（推送时间不能独自证明持续核心维护）。用稳定版对照现成扩展，不将master当作已验证发布版。此版本发布时间较早是实际选型因素，不能用“最新稳定”掩盖。

官方下载便携64位ZIP位于tools/aria2-rpc-probe，SHA256 67d015301eef0b612191212d564c5bb0a14b5b9c4796b76454276a4d28d9b288。release资产API无digest；这里只记录本地摘要，不称已比对发布方签名。仅在项目内运行，无全局安装/注册。

## 实测

scripts/probe-aria2-rpc.mjs 使用真实aria2c进程、随机RPC端口/密钥、项目内独立HOME/APPDATA/TEMP、配置/session/download目录。禁用DHT/LPD/PEX，不连接公网P2P。受控本地HTTP文件256 KiB。

- addUri设置pause=true并指定GID，tellStatus为paused且服务器零请求。
- 相同GID重复addUri被拒绝；调用方需query已有GID，不能把重复错误当成已新增任务。
- unpause后完成真实下载，文件逐字节一致，只有一次HTTP请求。
- 另一个paused任务saveSession后正常停止进程，再启动时同GID仍为paused、没有额外请求；unpause后文件一致。
- 使用libtorrent官方有效夹具作暂停导入比较：v2_only.torrent被RPC拒绝，v2_hybrid.torrent被接受为paused。此项只证明导入结果；不代表hybrid的v2侧下载或完整BT互通通过。

证据analysis/aria2-rpc-probe.json、logs/aria2-rpc-probe.log。passed=true指上述RPC控制/HTTP恢复检查通过，不表示引擎等价替换验收。

## 现成扩展

Aria2 Explorer官方master源码已只读导出至analysis/exports/aria2-explorer-background.js、aria2-explorer-rpc.js。manifest为MV3。捕获路径先chrome.downloads.cancel，然后调用download/send2Aria/addUri；本机提交失败会重新chrome.downloads.download原URL。它可传Cookie/UA/Referer，能降低基础集成工作，但这不是当前自研prepare/cancel/commit持久恢复协议的直接替代品。尚未安装该第三方扩展或完成其真实浏览器联测。

RPC层可支持先paused创建、后确认再unpause；整个浏览器交接仍需GID持久化、确认丢失查询、取消证据和Qt统一任务同步。直接兼容现成扩展与自研可靠交接可以分别验证，不能混为同一个验收结果。

## 结论边界

Qt+aria2能复用RPC生态，是值得比较的路线；当前稳定版纯v2夹具被拒绝，因此不能直接替换已验证的libtorrent纯v2能力。ed2k/Kad仍需独立方案。尚无同等Release构建下的包体积/内存/启动时间比较，也未验证libaria2的MSVC内嵌构建。

来源：[官方发布](https://github.com/aria2/aria2/releases/latest)、[RPC手册](https://aria2.github.io/manual/en/html/aria2c.html#rpc-interface)、[Aria2 Explorer](https://github.com/alexhua/Aria2-Explorer)。
