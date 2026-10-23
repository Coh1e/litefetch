# ed2k Qt 设置及主窗口入口

2026-10-20。新增 `downloader/src/ed2k_settings.*`，只使用现有 Qt Widgets/Core/Network；唯一 UI 仍为 Qt，未新增外部库或后台进程。

主窗口工具栏新增“ed2k 设置”。默认关闭，可配置服务器 IPv4/端口与 ASCII 昵称；明确显示实验性、4GiB 以下、无 Kad/回调/加密/入站及真实网络兼容待验证。客户端 hash 在首次配置时生成并随设置保持，不显示或要求用户手填内部身份。

配置写入当前任务目录 ed2k.json，使用 QSaveFile 原子提交并禁用直接覆盖回退。启动重新读取、验证版本/字段类型/端口/hash/编码与大小上限；坏配置保留原文件，ed2k 关闭并提示重新设置。保存失败恢复旧运行配置。关闭保留服务器信息和身份供以后启用。

TaskManager 的 configureEd2k 在 ed2k 工作线程存在时拒绝修改，避免工作线程读取配置发生数据竞争。保存后的设置立即用于之后启动的任务。运行中需先暂停，暂停/继续仍使用现有任务机制。

原来 main.cpp 无条件拒绝 ed2k 的入口已替换为未启用时提示设置；启用后“添加下载”按原有路径选择与无覆盖规则添加任务。任务类型显示 ed2k，暂停按钮允许该协议。配置在 TaskManager 构造前加载，以便重启后保留队列任务使用相同运行配置。未添加资源搜索或迅雷加速网络。

`ed2k_settings_test.cpp` 实际创建 Qt 模态窗口、操纵启用/地址/昵称/保存/取消，验证坏 IPv4 和未协商昵称编码拒绝、稳定身份与配置重载、关闭保留端点、取消无保存和坏 JSON 拒绝。任务测试增加运行时配置修改拒绝与暂停后允许。

构建 logs/downloader-build-ed2k-settings.log 成功。专项日志 logs/ed2k-settings-tests.log 为 13/13 通过（3.91秒）。实际设置截图和主窗口截图已经查看，无文字裁切：

- downloader/out/verification/ed2k-ui/settings.png
- downloader/out/verification/ed2k-ui/main.json.png

主程序使用隔离测试任务目录启动并加载显式回环服务器配置，main.json 记录 Windows 平台、窗口可见、ed2k_enabled/configuration_valid/settings_available 均 true。启动没有排队下载任务，不访问公网。图像采集是程序自身 Qt grab，没有读取其他应用。

限制：设置窗口、主程序配置加载、任务 API 和受控 TCP 下载分别有验证，尚未执行从主窗口手工填写链接到真实公网下载的整体互操作验收。UI 开放为实验性不代表 Kad、回调、扩展或任务崩溃恢复完整完成。主目标仍未完成。
全量回归 logs/downloader-tests-ed2k-settings.log：23/23 通过（45.33秒），覆盖 Qt 设置、任务、HTTP/恢复、BT/文件选择和浏览器原生桥接。
