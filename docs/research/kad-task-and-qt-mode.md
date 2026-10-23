# Kad 模式接入任务与 Qt 设置

2026-10-20，扩展现有 Ed2kRuntimeOptions、ed2k_task、ed2k_settings、main 和已有测试；产品仍只有 Qt UI，未新增第三方依赖、wxWidgets、aria2 或独立进程。

运行配置新增 useKad、最多8个明确bootstrapSeeds和发现总超时。validEd2kOptions 按模式校验：服务器模式需要服务器；Kad模式需要有效IPv4/UDP端口的seed列表，不依赖ed2k服务器。runEd2kTask 按选择调用已有服务器或Kad协调器，沿用同一SQLite队列、工作线程、进度、缓存、暂停/继续、发布与完成确认。运行中禁止修改配置，暂停后允许；全局模式会用于之后启动/恢复的任务。

Qt设置新增“找源方式”选择（服务器/Kad实验性），按模式显示服务器或引导节点字段。界面编辑首个Kad引导节点；如果已有多seed配置，更新首个保留其他项，清空地址会清除列表。不会下载或内置不明公共节点表。当前要求用户明确填写IPv4节点及UDP端口，默认不开启实验性下载。

ed2k.json 写为版本2，保存mode、kadSeeds、kadTimeoutMs，客户端身份和其他字段保留；仍支持读取版本1并按服务器模式解释。JSON类型、列表数量、地址、端口和预算均校验，原子保存。关闭总开关时运行配置强制停用Kad和服务器找源，但磁盘配置保留节点与身份。

Qt测试新增实际切换模式、空seed/IPv6拒绝、Kad配置重载、关闭保留seed但不激活，以及version1身份兼容。真实UDP/TCP连续测试增加TaskManager场景：在统一队列添加ed2k链接，实际Kad引导/查找/源查询、peer握手授槽、600000字节多批下载、内容验证发布并写入Completed。并非只调用协调器代替任务集成。

构建 logs/downloader-build-kad-task-ui.log 成功；专项 logs/kad-task-ui-tests.log 为18/18通过（6.07秒）。实际Qt设置截图 downloader/out/verification/kad-ui/settings.png 已查看，中文/字段无裁切。主程序使用隔离的无任务测试目录加载version2 Kad配置，main.json记录ed2k_source_mode=kad、配置有效、窗口可见与截图成功；此启动不发起公网请求。

限制继续明确显示：实验性、仅直连且<4GiB、无回调/加密/入站，真实公网兼容待验证。长期节点维护/持久化路由、Kad Hello/密钥、防火墙与混淆、即时取消、并行调度、更多故障及公网eMule/aMule互操作仍未完成。用户入口开放不等于成熟Kad客户端。没有关键词资源搜索、迅雷加速网络或后台后端进程。

此记录更新此前“Kad模式尚未接任务/UI、入口关闭”的阶段描述；整个原始目标仍未完成。
全量回归 logs/downloader-tests-kad-task-ui.log：28/28通过（47.76秒），覆盖Kad/ed2k、Qt设置、SQLite任务、HTTP/恢复、BT/文件选择与浏览器原生桥接。
