# 活跃目标与验收台账

目标：完成迷你迅雷功能逆向及已确认的新下载器范围，明确不做迅雷加速网络。目标未完成。

本文是当前状态索引，不是完成证明。每项必须由对应源码、原始证据或运行结果支持，不能以空窗口或合成任务代替真实下载验收。

## 逆向

| 要求 | 当前证据 | 状态 / 仍缺什么 |
|---|---|---|
| 原包解包与身份 | target SHA-256保持4bc3e1d193187fd22d82233253764754f417f0a4b239dbc5c8eb36be61f28617；PE/文件清单、解包组件哈希 | 已有静态清单 |
| 主程序任务分流 | minithunder-ghidra/functions.jsonl（7195个函数记录）、selected.jsonl、0043d8d6 / 0041a68b / 0041dffc等 | 已有分类/提交伪代码；间接COM调用与部分边界尚待闭环 |
| 引擎动态加载/ABI | minithunder-engine-abi.md、minithunder-engine-callpaths.md、minithunder-engine-internals.md；248项内部函数导出 | 66项主引擎绑定、输出对象句柄、普通/BT/ed2k创建恢复和磁力解析路径已确认；完整结构体、网络算法和运行行为未完成 |
| 状态机、队列、持久化 | state-resources报告、TaskDb SQL、004503b1初始化 | 仅部分，状态值/事件顺序和真实数据库尚未完整验证 |
| HTTP分段/恢复/限速/代理 | 引擎导出、槽位引用、包装与配置伪代码 | 部分；算法、资源变化与故障行为未完成 |
| BT、磁力链、ed2k/Kad | 导出与新建/恢复包装；磁力入口分类 | 部分；原版GUI磁力真实路径、协议内核和实际下载仍有缺口 |
| Chrome自动接管 | browser-native-bridge.md、browser-agent-ipc.md：点击JS→NPAPI→COM→共享内存/窗口消息或启动参数任务文件→主程序解析/提交 | 静态交接链已连通；真实接管、浏览器取消与失败恢复尚未动态验收 |
| Firefox自动接管 | 同上：点击、Cookie/来源页、CallThunder→COM，TYPELIB及接口槽位已核对 | 静态交接链已连通；原浏览器运行行为尚未动态验收 |
| IE自动接管 | browser-ie-interception.md、browser-agent-ipc.md、browser-context-engine.md | COM包→主程序重建socket/预读缓存→set_addition_info→引擎代理已定位；最终消费者、旧_infow文件兼容性、取消/恢复竞态与真实传输未完成验证 |
| 现代浏览器对应能力 | browser-capture-modern.md；downloader-browser-real-chrome.md；MV3扩展、Native host与Qt桥 | Chrome for Testing 155真实自动接管、禁用/离线回退、worker恢复及Qt重启恢复通过；正式版安装、Edge/Firefox和登录上下文未完成 |
| 排除迅雷网络 | 原DLL混合标准协议与P2SP，已决定新产品不分发/加载原DLL | 架构约束明确；最终还须审计源码、依赖、包内容及运行流量 |

## 新版实现与新增能力

| 已确认范围 | 当前状态 | 完成时所需证据 |
|---|---|---|
| 开源、Windows优先、Qt Widgets/现代C++ | Qt6.11.2与C++23、libcurl、SQLite开发版已构建部署；许可证未锁定 | Release可复现构建、完整开源材料和依赖许可证 |
| HTTP/HTTPS/FTP | libcurl8.22.0已接GUI；真实HTTP/被动FTP、404/截断/取消、强ETag条件Range续传、资源变化及范围核查已有测试 | HTTPS/TLS、认证边界、代理、Last-Modified/FTP续传及分段仍未完成验收 |
| BT/磁力链 | libtorrent2.1.2已接统一GUI/队列；SQLite缓存种子和选择，真实v1/v2/hybrid传输、磁力元数据→选择→数据、v1修复及原种子移走后的v2恢复通过 | 做种策略、代理/限速设置、公网发现、hybrid故障恢复和更多UI端到端验收未完成 |
| ed2k/Kad | 无wx、同进程；服务器与Kad找源已接Qt设置/统一任务，受控UDP→TCP验证引导/查找/源查询/授槽/文件校验发布，支持串行多源part、hashset、缓存和发布恢复；另已真实对接aMule3.1.0完成600KB、跨part及精确part边界下载，协调器与源SHA256一致，见 ed2k-real-amule-interop.md | 其他客户端/公网兼容、真实Kad/服务器、持久路由/长期节点维护、Low ID回调/入站、扩展/加密、并行调度、上传、完整大文件支持及崩溃遗留文件回收仍缺；保持实验性，不以基础直连互通冒充完整引擎 |
| 统一粘贴/拖放/批量入口 | GUI单条链接/磁力、Ctrl+O打开种子、种子内容检查和BT哈希别名去重已接入；ed2k仍仅解析 | 批量/拖放交互、ed2k创建和全协议重复处理未完成 |
| 暂停/继续/重试/删除/打开 | HTTP/BT暂停/继续、重试、取消、移除记录保留文件、打开文件/任务目录已接入；BT等磁盘暂停确认后保存并结束，取消保留BT数据 | FTP续传、ed2k生命周期、删除文件选项及完整界面交互未完成 |
| 重启/崩溃恢复、一致性 | SQLite记录与HTTP检查点恢复已实现；强制终止真实HTTP传输后Range恢复并逐字节验证通过；ready清单识别rename后未记完成的文件，目标变更拒绝覆盖 | 全应用更多崩溃时间点、断电/硬件故障及跨协议一致性仍未验收；清单清理/凭据保护仍待完善 |
| HTTP自适应分段/连接复用/站点上限/回退 | 未实现 | Range/非Range、限流、慢连接、重试与文件一致性测试 |
| 队列/优先级/并发/全局限速/低占用模式 | 普通下载持久化队列、优先级、并发上限已接入并测试；存储失败阻止新传输 | 跨协议调度、全局限速/低占用、站点公平性及连接池未完成 |
| BT文件优先级/上传与做种；ed2k上传/诊断 | 原生文件选择/跳过/优先级已接入、SQLite保存、等待磁盘确认后应用；真实选择下载和无选择禁用通过；会话上传限额配置已写，尚非用户设置 | 活动任务动态文件编辑、限速/公平性、上传做种策略、ed2k上传及端口诊断未完成 |
| 错误分类/恢复操作 | 普通传输基础错误分类/提示及从头重试已接入；HTTP/截断/存储失败测试通过 | 用户可理解的完整错误说明、认证/代理/TLS/协议特有错误和恢复操作未完成 |
| 中文、高DPI、键盘、托盘、完成通知、详情 | 只有环境窗口中文/平台插件通过 | 完整交互和多DPI/键盘/托盘/通知验收 |
| 系统/自定义代理及范围说明 | 未实现 | 各协议DNS/HTTP/对等连接覆盖核查 |
| 同名、空间、SHA256、分类保存 | 目的地重复检查和已有文件保护已测试；其他未实现 | 同名处理选项、空间/SHA256/分类保存与真实文件系统边界 |
| 浏览器扩展与上下文交接（第二阶段） | Chrome for Testing 155真实扩展→host→Qt接管、禁用及离线回退通过；原版交接已有静态函数链 | 正式版安装体验、Edge/Firefox、登录上下文、崩溃恢复真实浏览器联测及原版动态交接仍缺；见 downloader-browser-real-chrome.md |
| 计划下载/时段限速/完成动作（第二阶段） | 未实现 | 可控时钟、任务完成/失败和取消倒计时验证 |

## 后续顺序

浏览器接管新增证据：browser-context-consumer.md 已定位普通任务附加信息实现、0x78后台消息、协议块/缓存二次复制和HTTP头识别；最终socket消费、浏览器取消与失败回退仍未完成动态验收。

2026-10-20：browser-socket-consumption.md 补上 +0x48 接口偏移、普通/chunked 专用数据管道、socket工厂 GUID/映射表及 WSASocketW 恢复入口；预读正文进入缓冲已有静态证据。异步读循环、最终落盘与真实浏览器取消/失败回退仍未验收。

同日：browser-prebuffer-read.md 补上普通正文提交顺序、chunked缓冲解析/补读和设备适配层指令。下游落盘、完整Winsock读循环、异常边界及真实浏览器交接仍需验收；现代扩展实现仍未开始。

现代集成实现进展：downloader/browser/handoff.mjs 已实现与浏览器无关的接管事务协调和持久恢复协议，故障测试见 logs/browser-handoff-tests.log。此项替代上文“实现完全未开始”的旧状态；尚无真实浏览器适配器、manifest、Native host或Qt IPC，端到端接管仍未实现/验收。适配器约束及限制见 downloader/browser/README.md。

后续实现：adapters.mjs 已提供 storageJournal、downloadsAdapter 和 NativeClient，覆盖存储异常、状态查询、消息乱序、超时、断线与重连；恢复记录补上taskId/租约/暂停所有权校验。真实API/Port目前使用测试替身验证，尚无manifest/worker、请求关联观察层、原生host或Qt IPC，也未安装扩展。最新测试日志同上。

请求观察层进展：request-observer.mjs 已实现原始/最终URL与时间窗关联、重定向链、同URL并发拒绝、缺失历史拒绝、缓存溢出隔离、认证/头上下文筛除和按来源显式许可。尚无真实浏览器事件注册与权限/时序验收，也没有Native host/Qt接通。默认来源策略关闭；登录上下文支持仍未实现，不能把保留浏览器当作该需求已完成。

扩展接线进展：已补MV3 manifest、worker事件注册/恢复alarm、站点设置弹窗与开发包脚本；默认关闭，准备期间禁用会阻止取消浏览器。暂停/取消前复核原始和最终URL。57项Node测试通过（含worker+API替身集成），worker/popup语法检查通过；9文件开发包已生成。尚未真实浏览器加载、注册Native host或接入Qt，Firefox适配与登录上下文仍缺，端到端接管不算完成。

C++桥接进展：native-downloader-host.exe已构建部署，真实子进程＋QLocalServer测试通过，覆盖长度帧、UTF8、来源许可、畸形输入、IPC响应错配和应用离线。生产Qt IPC服务、持久prepare/commit事务、host注册以及真实浏览器下载仍未完成。详见downloader-native-host.md。

持久接管进展：schema4的browser_handoffs和TaskManager准备/查询/作废/提交接口已实现，commit与Queued任务原子保存。独立存储测试通过，覆盖回滚、重复/到期/冲突、并发连接和真实进程终止恢复。详见downloader-browser-handoff-store.md。生产IPC服务/GUI入口、host注册与真实浏览器联测仍缺。

本轮首次完整回归中BT对话框优先级用例失败一次；与存储用例分开记录。已增加具体返回值诊断，单独重复30次通过，尚未复现或确认根因，不标记为已修复；日志downloader-tests-with-handoff-store.log及downloader-tests-torrent-ui-repeat.log保留。

应用IPC接通：BrowserBridge已通过显式开发配置接入GUI代码；真实host→生产服务类→SQLite→默认传输队列→受控HTTP256KiB文件逐字节通过，重复提交不重下、准备/作废不发HTTP。长连接stdin预读问题已由失败测试复现并通过Unbuffered修正。详见downloader-browser-bridge-integration.md；浏览器安装/注册、真实事件、GUI配置体验、Firefox和登录上下文仍缺。

安装准备：已固定开发扩展key/预计ID并重新打包，生成指向当前host二进制的清单、配置预览与Chrome/Edge注册计划；未修改注册表或用户浏览器。61项Node测试、来源归一化真实host测试和host到HTTP整链通过。实际浏览器ID核对、原生设置入口及注册/联测仍缺；项目外HKCU配置须明确授权。详见downloader-browser-connection-artifacts.md。

原生设置：Qt工具栏已提供浏览器接管开关/目录选择，QSaveFile保存及普通启动重载已实现；关闭后保留IPC恢复通道，新prepare被拒绝，既有事务可完成。真实对话框测试、截图及部署包启动通过，完整C++回归10/10通过。host配置复制、浏览器安装/注册、真实事件和登录上下文仍缺。详见downloader-browser-native-settings.md。

原生连接准备：新增独立按钮导出与GUI实际设置一致的host运行配置和manifest；重复稳定、异配置拒绝覆盖及真实窗口测试通过，完整C++回归10/10通过。Chrome临时注册计划与操作前只读记录已备好，未写注册表；须获准项目外配置例外后才能做真实浏览器联测。详见downloader-browser-prepare-ui.md。

真实Kad对端基础验证：生产bootstrap/lookup API接受隔离aMule3.1.0的明文应答，空路由表返回0联系人，随后两条TCP文件路径均完成；最终参考进程退出。首次缺nodes.dat触发参考daemon自动下载公共节点表，已记录失误并以合法空节点表阻止该路径。见kad-real-amule-interop.md；非空/压缩报文、公网文件源及长期路由仍缺。

Kad压缩接收：新增E5有界zlib规范化，bootstrap/lookup/单页与多页源均接通，规范明文摘要去重；复用既有zlib（明确直接依赖），安装包总数61、Boost44不变。完整回归31项通过，新增边界测试数据错误修正后5项Kad各连续3次通过；见kad-bounded-compression.md。真实非空压缩对端及公网路由仍待验证。

Kad查询容错：真实UDP复现并修正同节点无关操作码先到导致查询失败；bootstrap/lookup/源收集匹配预期操作码，无关帧仍计入预算、不重置截止时间。完整32/32回归通过，见kad-reply-opcode-matching.md。源码确认aMule无条件拒绝127/8路由联系人，因此本机节点表方案未取得真实非空应答证据，未配置公共节点，相关兼容验证保持未完成。

批量/拖放入口：多行最多100项、规范来源去重及格式预检、批量公共保存目录、种子多选和主窗口文本/文件拖放已接入既有添加流程；Qt事件测试通过，无额外依赖PATH的部署包启动报告通过。见downloader-batch-and-drop.md；尚未批量真实下载端到端验收，冲突预览及批处理体验仍可改善。

批量真实HTTP验证：prepareBatch已供GUI与集成测试共用，新增总字符预算与原始错误行号；两个真实HTTP任务/SQLite入队/逐字节落盘、规范来源去重及无效批次不启动均连续3次通过，见downloader-batch-and-drop.md。尚未覆盖模态GUI全过程或混合协议批量。

托盘/通知入口：Qt收起与恢复、菜单退出、终态完成通知/状态栏已接入；关闭窗口仍退出，托盘不可用不隐藏。Qt事件测试与Windows独立启动报告通过，截图已查看，见downloader-tray-and-notifications.md。尚未观察实际系统通知投递，不把此项写成完整验收。

共享限速首阶段：HTTP普通/续传、FTP接入同一个下载正文预算，Qt提供明确覆盖范围的运行期限速入口；双128KiB真实任务在合计128KiB/s下2.13秒完成，取消/动态不限速验证通过，完整35/35回归通过。见downloader-http-shared-limit.md。BT/ed2k及上传预算、跨核心分配、持久化仍缺，不记全局限速完成。

BT运行期限速：TorrentEngine/TaskManager/Qt已接入会话共享上下行速率变更，真实session设置、低速下载后活动会话解除限额并完成验证通过，BT集成及对话框2/2通过；见downloader-bt-live-limits.md。HTTP与BT仍独立计额，ed2k、跨核心分配、持久化及上传吞吐尚缺。

ed2k共享预算：发数据请求前裁剪额度，与HTTP/FTP共用预算；小额请求、缓存恢复仅计缺失数据和取消验证通过，真实aMule600000字节在256KiB/s下2583ms完成，参考进程退出。界面按用户Everything风格偏好收敛为菜单/短工具栏/筛选/紧凑列表，Windows截图已查看；完整35/35通过。见ed2k-shared-download-budget.md与downloader-compact-ui.md。BT跨核心总额分配、ed2k上传、持久化及其他需求仍未完成。

限速持久化：bandwidth.json保存HTTP/FTP/ed2k共享下载及BT上下行限额，恢复先于队列调度；严格格式/范围/大小校验、原子保存，坏文件保留并默认回退提示。设置/限速器2项测试及有效/损坏配置独立启动检查通过，见downloader-bandwidth-persistence.md。跨核心总额分配、低占用及上传等仍缺。

低占用预设：选项菜单/Ctrl+Shift+L，HTTP/电驴下载与BT下载各至多128KiB/s、BT上传至多32KiB/s；保留更低原值、关闭恢复，模式持久化。设置/限速器2/2及3种独立启动检查通过，见downloader-low-usage-mode.md。固定分组预设不等于动态跨核心总额分配，其他范围仍保留。

任务列排序：新增真实数值排序，更新整行时暂停排序，按ID恢复选择，筛选隐藏选择及移除后不误选邻居；真实Qt50次重排和字段身份/筛选测试连续3次通过，独立启动3场景通过，见downloader-task-sorting.md。列设置保存、大规模性能和其他范围仍待继续。

原版接管异步接收静态补证：只读导出51个已有函数及原指令，纠正包装器getter的栈参数归属，定位raw虚表接收槽/WSARecv/pending分支，8项原PE字节核查通过；见browser-socket-read-dispatch.md。管理器+74间接目标、原版取消时点、下游写盘及动态一致性仍未闭合，没有运行闭源样本。

原版管理器工厂静态补证：追到10447a88初始化的服务查询键、偏移4接口映射、两张虚表+74共同工厂及其返回偏移0适配接口，20项PE字节核查通过，64个已有函数只读导出；见browser-socket-manager-factory.md。服务选择、COM到native接收桥、取消/预读/落盘动态一致性仍缺，未运行样本。

原版COM/native桥接补证：包装器构造/原生指针+10绑定、QI返回偏移0、COM+0c thunk的this+4调整、读取调整后+0c并调用native+4已连到此前WSARecv路径；29项静态字节核查及原9项通过，73个已有函数只读导出。见browser-socket-com-native-bridge.md。运行时选择、回调、取消及下游发布一致性仍未完成。

原版完成分派补证：native完成入口/活动清理→COM操作+8 handler→注册用户或拦截记录+0c的本地静态路径已补，39项PE字节核查通过；见browser-socket-completion-dispatch.md。另排除把管理器+2c线程分派误当操作工厂的方向；0x401接收、具体管道身份、取消竞态和最终落盘动态验证仍缺。

原版具体管道回调身份：公共基类代理+20保存管道、操作上下文+44设置/读取、代理+0c→公共分派→管道+4已映射到普通10099e6f和chunked10097c8e；56项PE核查通过，123个已有函数只读导出。见browser-socket-pipe-callback-identity.md。动态服务选择、0x401接收、取消/失败回退、下游落盘和文件一致性仍未完成。

原版窗口完成分支：已定位10038940创建内部隐藏窗口并注册10038810，原先已有函数元数据表明该过程引用1003f660/CancelIo等，但0x401具体分支尚未核查。追加只读导出因自动审批审核额度限制未执行，没有绕过；见browser-socket-window-completion.md。其他产品范围仍可推进，目标保持未完成。

原版0x401路径重试补证：同一只读导出审核恢复后成功，窗口过程将wParam操作指针交1003f660；存在以VC运行库标题查询后跳过本次分派的条件，113h定时器分支有CancelIo。64项PE核查通过，131个已有函数只读导出；见browser-socket-window-completion.md末尾。动态接管、取消竞态和最终文件一致性仍未完成。

用户摘要校验首阶段：完成的HTTP/FTP/ed2k单文件可从任务/右键菜单输入SHA-256，后台64KiB流式读取、可取消，Windows普通并发写入拒绝；已知向量/失败/取消与真实Qt输入到结果提示通过，完整38/38回归通过。见downloader-sha256-verification.md。自动校验、任务摘要持久化与发布保护、BT多文件支持仍待接入。

BT逐文件用户摘要：完成的BT任务可从同一菜单选择根目录内文件，规范路径归属预检后用后台SHA-256核心校验，输入框显示文件名；目录外/父跳出/目录拒绝及所选文件真实摘要、空文件向量测试通过，校验核心/Qt对话框2/2通过，任务表Qt测试通过。见downloader-sha256-verification.md末尾。整任务批量摘要清单及自动发布前摘要仍未实现。

继续补足原版关键函数链和浏览器接收端，记录保留/改进/排除映射；据此实现统一模型、下载核心及界面。环境窗口不算产品完成，静态导出不算协议动态验收，第二阶段项不被默默删除。
# 2026-10-21 HTTP 磁盘预检

逆向缓冲批次执行进展：10060c90创建1006bb00对象后，直接以其+4接口+c进入1006bb90，已核对接口表和this调整，修正“调用即新建后台线程”的未证实表述。后续有批次组织、状态通知和待处理记录清理，尚不能当成物理写盘完成。原PE138项核查通过，具体WriteFile/完成回传与动态一致性仍缺，详见browser-downstream-numbered-factory.md。

逆向文件服务进展：确认10447ab0持有对象+4保存初始化按GUID103704cc查询的接口，找到同GUID的本地CFileIoSystemImp主表103781dc及+2c实现10061230。该层先处理缓冲并可创建后台工作对象，不是立即WriteFile或持久化成功。原PE132项核查通过；实际服务选择、缓冲工作对象到系统写入及动态一致性仍待闭合，详见browser-downstream-numbered-factory.md。

逆向默认接收器进展：数据管理工厂+0c对应100dd960，默认接收器表10380624+4为100efbc0；构造来源连到data_io_transfer(100b05f0)，其100b0a00通过全局10447ab0持有服务的+2c提交。该操作句柄未等同Windows HANDLE，服务到WriteFile及完成路径未闭合。原PE121项核查通过，详见browser-downstream-numbered-factory.md。

逆向正文管理进展：文件管理100dc080一分支构造100e05a0对象并存+78，表1038003c+18指向100e1600，已确认该分支提交到data_file_handler接收器列表。反向定位两个WriteFile包装入口，但接收器具体实例/文件句柄仍未连接。原PE112项核查通过，原版动态写盘未验收，详见browser-downstream-numbered-factory.md末节。

逆向QI/缓冲消费进展：data_file_manager的100da100对目标GUID返回未调整主接口，+14(100d6410)同步构造自有字符串再转交+24(100dc080)。同时确认chunked的+60调用取自管道+148而非普通正文的+150，纠正接口层次混同。原PE105项核查通过，239个已有函数反编译完成；浏览器上下文实例配对、更下层写盘和原版动态一致性仍未闭合，详见browser-downstream-numbered-factory.md。

逆向持有对象进展：构造表赋值确认子任务handler位于+98，消息13读取+60即完整对象+f8；具体上层创建路径将事件处理器+138传入该字段，而+138由data_file_manager（100da5c0，主表1037fba8）构造返回。原PE核查94项通过。尚未证明QI返回接口/this调整及与浏览器管道+150的对象配对，最终落盘仍未闭合，详见browser-downstream-numbered-factory.md末节。

2026-10-22逆向索引分派进展：找到子任务handler100b6890消息13h的两级跳转目标100b69f6，从持有对象查询GUID271151ca1fc8d940ad3c5e543766669e写输出；原PE核查总86项通过。新增静态GUID扫描仅在引擎找到两份副本，未找到既定映射形态。仍是候选，尚未与管道+150对象身份及最终落盘连接，runtimeVerified=false。详见browser-downstream-numbered-factory.md末节。

上传状态展示：BT核心累计上传/payload速度经进度和结束结果传到任务，停止清速度，继续/重试清旧实时状态。做种速度列显示↑，详情显示最近累计上传及速度，未知值不冒充0；SQLite不重复存统计。真实4MiB上传计数/非零速度及任务传播断言通过，完整41/41回归80.50秒通过。填充产品界面的视觉/交互及跨重启精确统计仍待验收，详见downloader-bt-seeding-policy.md。

2026-10-22真实做种比例证据：本机v1完成后暂停原始来源，独立leecher禁用发现/PEX且仅连接下载器端口，收到完整4MiB并逐字节一致；核心仅100%比例条件正常Completed，非计时退出。构建及BT相关3项58.01秒通过。v2/hybrid比例、多下游、跨重启与独立做种并发仍待完善；没有引入迅雷加速网络，详见downloader-bt-seeding-policy.md。

2026-10-22重新做种入口完成一轮验证：上次审批额度导致未执行的最终构建已按同权限成功重提；启用做种条件后可对已完成BT重新校验并做种，复用ID/文件选择/恢复数据，0/0不重启。BT相关3项44.45秒通过，完成HTTP防误启动及任务测试0.61秒通过。尚无真实下游上传比例达标及零重下证明，完整目标继续，详见downloader-bt-seeding-policy.md。

BT做种策略应用接入：选项菜单原生设置，项目内bt-seeding.json严格校验/原子保存，启动恢复；TaskManager在启动工作线程前复制策略，已运行任务不受设置变更影响。实际Qt有效/损坏配置启动检查通过，完整41/41回归通过（66.78秒）。真实上传比例达标、做种并发槽、已完成记录重新做种等仍需继续，详见downloader-bt-seeding-policy.md。

BT做种策略核心：TorrentRunOptions可指定完成后的时间/上传量相对所选大小比例，先达到任一限制停止，默认均0仍完成即停止。计算测试及真实BT完成后保持至少1秒做种进度的集成测试通过，BT相关3项41.72秒。TaskManager/原生设置/持久化、真实上传比例与跨重启时间尚未接入验收，不能称用户功能已完成。详见downloader-bt-seeding-policy.md。

ed2k中途存储失败证据补齐：回环来源传完剩余分片后以目录阻挡新缓存文件，确认StorageError向上传递、当前连接关闭、备用来源未连接、目标未发布且先前完整缓存逐字节保留。新场景首轮及连续3轮通过（重复总4.15秒）。未模拟实际写满或完整GUI任务链故障，详见downloader-task-details.md。

ed2k错误分类后续：明确的暂存/缓存I/O失败用StorageError，已有目标/发布冲突用DestinationError；协调器不再吞掉这两类错误换源，任务映射Disk/Destination并保留原因。构建及含真实受阻目标路径、保留已有文件用例的ed2k14项通过（4.59秒）。实际写满及网络中途失败时序仍未验收，详见downloader-task-details.md。

BT错误分类后续：状态错误不再全归磁盘，按libtorrent错误码及error_file组件区分TLS/磁盘/元数据请求/网络/内部异常，add_torrent同用分类。无新依赖，构建及新增映射、BT集成、BT界面3项测试通过（36.74秒）。辅助方法错误与真实异常注入尚未全部细分验收，详见downloader-task-details.md。

错误分类回归结果：版本3浏览器交接测试夹具补删新列，避免伪造旧结构；数据库在BEGIN IMMEDIATE后读取版本以串行化迁移检查。修正后完整39/39通过（59.39秒，logs/downloader-tests-error-types-full-fixed.log）。初次38/39及原因保留在downloader-task-details.md，不以重跑掩盖失败。

错误分类后续：Task新增结构化errorType，数据库模式5原子迁移新增error_type，旧记录保留原因且不猜分类；工作线程失败保存类型，继续/重试清空。详情按类型给建议。构建及含版本4/版本1迁移和失败重开测试的download-tasks通过。后端粗粒度映射和详情完整交互仍待核查，详见downloader-task-details.md。

界面详情补充：新增任务菜单/右键“任务详情”、Alt+Enter及双击入口，原生Qt只读快照展示完整错误原因和目标/字节等信息，HTTP/FTP隐藏URL密码；无新依赖或常驻按钮。构建通过；结构化错误分类尚未贯通持久化，窗口完整交互未验收，详见downloader-task-details.md。

Everything方向界面补充：任务表支持拖动列顺序，改变列宽/排序后正常退出保存项目内task-table.json，下次启动恢复；名称列仍自适应，筛选不保存。构建及有效/损坏配置两个真实Qt启动探针通过，原配置均未被无交互启动覆盖。证据及未验收交互限制见downloader-compact-ui.md列布局节。

运行中空间检查补充：HTTP/FTP正文写入路径每约4MiB或1秒重新查询；未知长度按64KiB小额余量检查，仍非空间预留。EOF未知长度和8MiB chunked精确内容用例加入；修正测试服务未排空发送队列即关闭的问题后，download-transfer通过、download-resume连续3次通过（26.59秒）。没有运行中实际填满磁盘的验收证据，详见downloader-disk-space.md。

逆向交叉排查：补齐父对象构造区间，未见ESI基址中途调整；局部+48接口地址使用只找到状态/查询调用，未找到选择槽调用。全程序CMP13候选中的1009d2d0已确认是FTP操作状态比较，排除为编号创建者。原始导出158个已有函数候选，具体工厂仍未确认，详见browser-downstream-numbered-factory.md交叉排查节。

逆向this核查：确认1007ece0写+48接口表10379974、该表+4直接指向1007a5c0且入口保存ECX；未发现该函数的直接调用者。上下文字段换算与+548候选的身份仍存在疑点，不能宣称具体工厂已定位。新增原PE核查3项，总80项通过，仅静态证据，详见browser-downstream-numbered-factory.md末节。

逆向上下文追加：补证1007a5c0当前ESI+500取值作为普通管道首构造参数，经1009a154/100bb6c0原样写管道+28；owner是另一个参数。偏移候选搜索含无关计数/缓冲字段，尚不能确定完整对象字段身份或编号13具体实现。原PE核查77项通过，运行时仍未验证，详见browser-downstream-numbered-factory.md追加节。

逆向后续：补证普通/chunked管道+150由编号13h的上下文创建入口输出；桥接1007a3f0读取上下文+8并经管理器+2c/同线程接口+0c分派。仍需识别具体上下文实现和落盘方法，不能称最终写入链闭合。只读导出及原PE字节检查73项通过，runtimeVerified=false。详见browser-downstream-numbered-factory.md。

后续BT补充：确认文件选择后、解除upload_mode之前按所选文件涉及的完整分片范围预检，边界分片合并避免重复计数。构建、空间计算测试和BT/BT界面2项集成测试通过（36.28秒）。已有数据尚不抵扣，恢复任务空间估算偏保守；低容量BT拒绝端到端实测仍缺。详见downloader-disk-space.md，未缩减整体逆向或新增范围。

后续FTP补充：通用curl写入回调在首次正文写盘前读取已知长度并检查空间；本机FTP巨大SIZE用例返回Disk、零字节写入且无目标/暂存残留，普通FTP/HTTP集成测试通过（logs/downloader-tests-ftp-space.log）。未知长度运行中检查及BT空间计算仍待完成。

后续补充：ed2k 恢复前检查完整目标暂存空间，接收分片前检查剩余目标及当前缓存分片的双份需求，缓存写入前再次检查；明确不足映射磁盘错误并保留恢复缓存。构建、相关15项测试、完整39/39回归通过（58.20秒）。BT/FTP空间预检及并发预算仍未完成，限制和证据见downloader-disk-space.md。

新增已知长度 HTTP 正文接收前的卷空间检查，正常续传按剩余字节检查，完整重下在截断前检查，明确不足时保留原暂存及清单。构建和 download-resume 回环测试通过；不足/保留/恢复准确性均有用例。查询失败、未知长度、空间竞争、其他协议尚未覆盖，详见 downloader-disk-space.md。整体逆向与新增范围保持，不引入迅雷加速网络。

