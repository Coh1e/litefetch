# 原生 ed2k 与 aMule 3.1.0 的实际文件互操作

日期：2026-10-21（本机时区）。本阶段首次使用独立的真实协议实现作为对端，区别于此前受控Qt fixture。

## 范围与隔离

参考程序是项目 tools/amule-3.1.0 中既有的官方aMule 3.1.0 Windows便携包。amuled.exe SHA256为6d3b1309153a9f43792e9d9c0ba3a199035600a4f24aa2b79b7b1584d0831c5e。源码/发行包来源及原始包摘要见此前ed2k-windows-spike.md。这里只把固定版本用作外部测试对端，不改变产品架构。

新脚本 scripts/probe-amule-peer-interop.ps1 创建独立run目录，显式把参考程序数据/控制端口绑定127.0.0.1，关闭服务器/Kad公网连接、自动连接、更新检查、GeoIP下载、UPnP和API后台。配置、合成共享文件、HOME/USERPROFILE、APPDATA/LOCALAPPDATA、TEMP/TMP均为该测试子进程的项目内路径，没有修改父进程/系统环境或安装服务、协议注册。结束通过amulecmd正常关闭参考进程，cleanup.json均确认退出。

产品不链接、启动或分发aMule/wxWidgets；探针download-amule-interop-probe只链接现有Qt/原生ed2k模块，不安装、不列为默认CTest。参考进程只由显式测试脚本启动，不是下载后台方案。

## 真实协议链路

探针计算合成文件的ed2k身份，与aMule实际共享列表读取的hash核对。连接真实aMule完成Hello、文件名应答，必要时FILESTATUS、STARTUPLOADREQ/ACCEPTUPLOADREQ，读取真实hashset、发送多批请求并接收数据块；part及整文件校验后发布，逐字节对比。

随后使用产品现有download协调器再次从同一真实aMule源下载，源provider携带实际peer userHash，要求TCP身份一致；复用真实协调器的part边界、缓存、内容校验及发布路径。探针不是用模拟数据调用parse函数代替网络，也不是把aMule作为产品下载后端。

| 文件字节数 | ed2k hash | hashset返回数 | 探针与协调器 |
|---:|---|---:|---|
| 600000 | f2c3c44e0f51c286d9a53a84c245d598 | 1（小文件直接身份） | 均完成，逐字节一致 |
| 9740345 | d25d39cdf8380a4f44b60e95113dbcba | 2 | 均完成，跨9728000字节part边界 |
| 9728000 | 8d25e10a9f21b10e25c845397e72ca26 | 2 | 均完成，真实验证精确边界额外空part规则 |

另外用PowerShell独立计算每次源文件、探针结果、协调器结果的SHA256，三者均相等。汇总analysis/amule-peer-interop-results.json，同时记录referenceExited和回环绑定日志。

具体成功证据：

- work/amule-peer-interop/20261020-223745-c3708c/interop-result.json（600000字节）
- work/amule-peer-interop/20261020-223757-a7874c/interop-result.json（9740345字节）
- work/amule-peer-interop/20261020-223818-e66a01/interop-result.json（9728000字节）
- 各目录含run.json、cleanup.json、reference/stdout/stderr、shared-files和探针日志；顶层日志为logs/amule-peer-interop-coordinator.log、amule-peer-interop-multipart.log、amule-peer-interop-exact-part.log。

## 发现与修正

首次探针在记录短应答时使用QByteArray::first(128)，数据小于128字节触发Qt断言，导致探针退出，没有得到完整诊断。改为安全的left(128)，并将本手动探针的CRT异常输出重定向到捕获日志后，真实传输成功。该问题只在新增探针的诊断代码，不是原生协议模块的兼容性错误；失败目录保留，不把失败抹成成功。

## 边界

这证明固定aMule 3.1.0与当前原生实现的基础明文直连上传/下载、大小文件hashset及part边界互通；没有验证Kad真实对端、真实ed2k服务器、混淆/扩展、安全身份、Low ID回调、多方公网、并行上传或其他客户端。公网网络特性仍不能宣称完成。产品保持实验性，原目标包括的逆向与其他新增内容仍需继续，明确不做迅雷加速网络。
