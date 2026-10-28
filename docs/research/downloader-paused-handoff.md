# 下载器目标暂停交接

> 历史暂停现场：2026-10-27 人类已恢复范围核对与重设计，完整目标保留，界面改为 Win32 原生。当前决定见 [重设计核对](../product-reset.md)。下文暂停状态、Qt 约束和恢复顺序仅供追溯；逆向地址与证据线索可按任务使用。

保存日期：2026-10-24（Asia/Shanghai）。用户明确要求暂停目标、保存进度和计划。目标未完成；恢复须由用户明确指示。暂停后不继续分析、实现或测试。

## 目标与约束

完整目标：完成逆向和之前明确的新增内容，明确不做迅雷加速网络。范围以 downloader-scope.md 为准，不以现有测试覆盖缩减要求。

- Windows 优先，Qt 6 Widgets 唯一 GUI，现代 C++23、CMake/vcpkg，CLion；Everything 式紧凑克制列表、键盘优先。
- 保留 libtorrent，只允许必要的最小 Boost 模块及传递闭包；不引入全量 Boost、aria2、wxWidgets。
- ed2k/Kad 同进程；aMule 仅隔离实验对照，非产品后台依赖。
- 不复用闭源迅雷 DLL，不做迅雷加速/P2SP 网络。原样本仅逆向参考。
- 所有产物、配置、缓存、临时文件留在项目。临时 Chrome 注册已恢复；Caffeine 保持暂停。

## 当前实现与验证基线

产品在 downloader/：Qt 原生列表、HTTP(S)/FTP、libtorrent BT/磁力、实验性原生 ed2k/Kad、SQLite 任务与恢复、Chrome 扩展/Native Messaging 交接。已有紧凑列表与快捷键、布局保存、任务详情及错误分类、增量空间检查、限速设置、BT 时间/比例做种策略及上传统计。各功能仍有范围限制，详见 downloader-progress.md、downloader-disk-space.md、downloader-task-details.md、downloader-bt-seeding-policy.md。

最近产品完整回归基线：logs/downloader-tests-upload-metrics-full.log，41/41 通过。后续本轮只改逆向脚本、导出和分析笔记，没有改产品核心。不能用该回归宣称全部产品范围完成。

## 最新逆向证据

- 主笔记：browser-downstream-numbered-factory.md；原始指令与伪代码在 analysis/exports/minithunder-socket-read/。
- 原引擎 SHA-256：38dd7f676a592bf1810e314e370b28fa4a43e2df3e6a8181447335333d1b0083。
- 最近原 PE 字节核查 230 项通过，runtimeVerified=false。这些是静态位置/指令核对，不是动态接管验收。
- 已静态连接正文接收、文件记录、缓冲批次、WriteFileGather 与备用 WriteFile、公共完成投递、io_manager 包装器/请求回调、文件范围和错误汇总、data_io_transfer 通知及析构移除路径。
- 已确认 data_io_transfer 的通知接口在后续正文管理初始化被补上；构造瞬间 +40 为空不能代表运行时为空。此修正已记入主笔记。
- 正文管理辅助表 1038002c 只有两槽：100e10d0、100dde50。100dde50 从辅助接口 this+4 的上下文再取 +8，向全局管理器 +28 发送 0x67、2、0x2714；没有读取传入的原错误码。
- 最近完整伪代码导出日志 socket-data-handler-file-error.log：450 个已有函数候选。socket-data-handler-error-pseudo.log 为后续独立临时解码，保留已有伪代码。
- ExportSocketRead.java 使用 PseudoDisassembler 临时解码数据库缺失/截断指令，不修改数据库。export-socket-read.ps1 新增 -InstructionsOnly，避免仅查指令时重做全部反编译。

## 暂停前最后只读发现（尚未纳入字节断言）

正在追踪全局管理器 +28 的本地实现 10037460。当前 socket-read-instructions.txt 已包含完整相关指令：

1. 以调用方上下文键查询记录；缺失返回 8000ffff。
2. 分配 0xb4 字节对象，10033a30 构造，表 10375a94；经 1003f520 建立通用操作。
3. 1003755e/64/6a 将三个载荷保存到操作 +a0/+a4/+a8；10037570 写操作类型 33339999。
4. 100375b1 调 100362c0 登记键与操作；100375cf 调 1003f6a0 进入前述公共完成投递链。

这些指令支持“事件被封装为异步操作投递”，尚未确认其最终处理函数、0x67/2/0x2714 的业务意义、实际任务实例和 UI 状态变化。10033a30 伪代码已存在，后续可从表 10375a94、上下文处理器与 33339999 分支继续，勿重新从头扫描。

## 恢复后的优先计划

1. 核对 10375a94 操作表、33339999 的接收/分派路径，以及事件载荷 +a0/+a4/+a8 的读取者。将本地错误事件连到具体任务状态处理，保留运行时身份与静态候选的区别。
2. 核对读缓冲接收者集合注册及接收者 +c 错误实现；补齐浏览器上下文、正文管理对象、文件服务真实实例的配对。验证取消时序、重复/迟到回调与最终文件一致性。
3. 开展原版浏览器自动接管动态对照：真实触发、浏览器取消、交接/失败回退、预读正文、最终文件；现代自研 Chrome 联测不能替代原版动态证据。
4. 继续既定产品缺口：HTTP 自适应分段/连接复用/站点上限；真正跨核心全局上传下载限速；代理和 TLS 对照；完整同进程 ed2k 上传、LowID、大文件、Kad 路由/持久化与真实互通；BT 独立做种队列和重启统计；恢复任务空间估算；计划任务/定时与完成动作；Edge/Firefox 及浏览器认证；许可证、依赖规模和开源分发审计。顺序可结合阻塞情况调整，完整范围不变。
5. 每项按实际语义验证，保持 Everything 风格，不因增加功能扩张工具栏和装饰。完成目标前按范围逐项验收，不以静态核查数或当前绿测作为完成证明。

## 运行和工具状态

暂停检查未发现 java 进程；最近 Ghidra 导出均已有完成日志，没有需要继续轮询的分析句柄。没有新批准请求。工作区无提交/PR 操作要求。

后续构建用 downloader/build-local.cmd；测试用 downloader/test-local.cmd。现有 vcpkg 访问和真实 TCP/UDP 回环测试可能需既有沙箱升级；不要在构建完成前跑旧二进制，不重复启动仍存活的进程。测试正则经 cmd 时不用竖线。

