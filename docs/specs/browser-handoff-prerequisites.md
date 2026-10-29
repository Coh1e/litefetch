# 二进制与本环境说明（#23/#24 前置）

## 发生了什么

Qt 遗留实现连同 `native-downloader-host`（浏览器 native messaging host）一起删除了：那个 host 是用
Qt（`QJsonDocument`、`QLocalSocket`、`QCoreApplication`）写的，构建在 `LITEFETCH_BUILD_QT_LEGACY` 分支里。
产品现在无 Qt，所以这个 host 需要**重写**，不是恢复。

协议本身没有丢，记录在：

- `docs/research/downloader-native-host.md` — host 的消息帧与本地 IPC 约定；
- `docs/research/downloader-browser-real-chrome.md` — 上一代用真实 Chrome 155 做过的联测记录（含当时的缺口）；
- `browser/README.md` — 扩展侧的方法与约束（prepare/query/commit/abort、64 KiB 帧、12 条在途上限）。

## 重写需要什么

1. **host 可执行文件**：读 stdin 的 native messaging 帧（4 字节小端长度 + JSON），把请求转成本机 IPC 交给主程序，再把结果写回 stdout；
2. **主程序侧的端点**：目前 worker/service 没有接收浏览器请求的通道，需要一个新的本机 IPC（命名管道最省事）；
3. **JSON**：项目里没有 JSON 库。可以自带一个最小解析器（只需对象/字符串/整数/布尔），不要为此引入依赖；
4. **注册**：HKCU 下 `Software\Google\Chrome\NativeMessagingHosts\<name>` 指向 host 清单（Edge 在 `Software\Microsoft\Edge\...`）。写入注册表属于需要人类明确授权的操作，项目里默认不自动注册。

## 环境准备清单

要让 #23 能真正验收，需要人类提供：

| 项 | 用途 | 状态 |
|---|---|---|
| Chrome 或 Edge（任意近期版本） | 加载 MV3 扩展、发真实下载请求 | **需要人类确认本机已安装** |
| 允许在 HKCU 写入 native messaging host 键 | 浏览器才能找到 host | **需要人类明确授权** |
| 扩展"加载已解压"操作权限 | 自动化脚本用 `Extensions.loadUnpacked` 需要以特定参数启动浏览器 | 可用项目的 `tools/chrome-for-testing`（若仍在）或人类手动加载 |
| 一个可重复下载的测试站点 | 验证"暂停浏览器 → 本机接管 → 提交" | 项目内的 HTTP 夹具即可，不需要公网 |

## 我可以先做的部分（不需要外部环境）

- 写 Qt-free host 与最小 JSON 解析；
- 写主程序侧命名管道端点与它的进程内测试（夹具代替浏览器）；
- 用项目内 HTTP 夹具验证"host 收到 prepare → 主程序建任务 → 下载完成 → query 返回 committed"。

**不能**在没有浏览器和注册表授权的情况下声称"真实浏览器接管通过"。
