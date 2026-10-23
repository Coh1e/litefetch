# ThunderAgent：COM 入队到 MiniThunder 的 IPC

2026-10-18。只做资源提取、TYPELIB 元数据读取与 Ghidra 静态分析；未加载执行样本、注册 COM 或启动原版。

## 证据文件

- `analysis/exports/browser-agent-ghidra/metadata.json`：ThunderAgent.dll 样本 SHA-256、装载地址及分析限制。
- 同目录 functions.jsonl、selected.jsonl、逐函数 .c、commit4-listing.txt。
- `analysis/exports/browser-agent-resources/.rsrc/REGISTRY/102`：ThunderAgent.Agent(.1) → CLSID `{485463B7-8FB2-4B3B-B29B-8B919B0EACCE}`，InprocServer32=%MODULE%，Apartment；TYPELIB `{26D657AE-A466-4F44-AB1D-5CFFFADBED97}`。
- TYPELIB 原始资源 `.rsrc/TYPELIB/1`，使用 `scripts/ReadTypeLib.cs` 经 Windows LoadTypeLibEx(REGKIND_NONE=2) 只读取元数据，没有 COM 注册/实例创建。导出 `analysis/exports/browser-agent-typelib.txt`。**宿主为64位，导出的 oVft 是宿主指针宽度8；目标32位偏移需除2。** 这并不意味着目标 DLL 是64位。
- `analysis/exports/browser-agent-vtable.tsv`：文件偏移 0x9e68 起31项函数指针，按4字节目标指针输出，可与 TYPELIB 索引独立对照。

## COM 方法与队列

| 方法 | TYPELIB 方法id | x86 vtable偏移 | 目标VA |
|---|---:|---:|---|
| AddTask4 | 11 | 0x44 | 10004bd9 |
| AddTask5 | 12 | 0x48 | 10001456 |
| CommitTasks4 | 16 | 0x58 | 1000542e |
| CommitTasks5 | 19 | 0x64 | 1000544d |

IAgent4 定义 AddTask4 参数：Url、FileName、Path、Comments、ReferUrl、StartMode、OnlyFromOrigin、OriginThreadCount、Cookie、CID、StatUrl。10004bd9 包装10004614；后者复制字符串、格式化数值字段，保存可选 socket/已收数据、TaskOption，并把节点加入对象任务链表（节点next偏移0x98）。实际客户端字段和对象内部偏移存在this调整，不应脱离调用约定直接把Ghidra变量名视为源码成员名。

AddTask5 在上述参数后增加 eCallType、GCID、FileSize。10001456 当前静态实现只在 eCallType==1 分支调用：尝试 InternetGetCookieW(URL)，成功时用系统Cookie替代传入Cookie，然后经vtable+0x44转 AddTask4；失败使用原Cookie。此函数内没有GCID/FileSize继续消费证据，其余eCallType返回0不代表下载入队成功。不能把接口存在视为每个字段都有效。

CommitTasks4 的1000542e没有被自动分析成独立函数，原始指令已导出。它将参数转交10005216，控制位为(1,0)。CommitTasks5 的1000544d暂存类型字段、强制设置为1（Mini）、调用同一核心，再恢复旧类型。Chrome JS调的是CommitTasks4(0,1)，TYPELIB分别命名nThunderType/nIsAsync；Firefox调CommitTasks5以明确走Mini。其0x80004005等HRESULT与业务结果输出分开，不能单凭HRESULT=0判断成功下载。

## 已运行目标：共享内存＋Windows消息

10005216负责目标窗口、序列化、通知和清理。10001ac7根据类型找目标；Mini时10006c34调用：

`FindWindowW(L"MiniXL_Thunder_frmCmdCenter", L"MiniXL_Thunder_backwnd")`

10002b48为任务列表分配命名文件映射（页文件支持）：

`MiniXl.Thunder.Agent.7106855B-07B9-48d5-B90F-42B604EE28BA_TaskData_%08x`

末尾是GetCurrentThreadId十六进制。主头部6个DWORD设置为 `2,0,1,0,param_1,taskCount`；每任务字段数为0x11=17。字段依次包括URL、FileName、Path、Comments、ReferURL、StartMode、OnlyFromOrigin、OriginThreadCount、Cookie、CID、StatURL、OriginExplorer、ProtocolInfo、RecvData、TaskOption、StatClick、CharSet。

100015dc 的UTF-16编码不是NUL结尾：先写DWORD字节长度，再拷贝该长度的字符串。二进制字段用100021a7进行长度＋数据写入。ProtocolInfo在有socket时使用WSADuplicateSocketW创建目标进程可用的0x274字节WSAPROTOCOL_INFO，RecvData携带已收到的字节；无socket时写零结构/空数据。这说明代理具备移交连接与预读数据的通道，具体哪些BHO触发它由BHO调用链决定。

序列化后10005216→10001aee：Mini消息号0x2410（完整版为0x2408），普通任务wParam=0x1100，媒体类路径为0x2200，lParam为当前线程ID。同步SendMessageW结果写入业务结果指针。发送后UnmapViewOfFile与CloseHandle，映射不留作长期数据库。

100032d1还会尝试启动/等待目标、使用Mini探活消息0x2411及3秒SendMessageTimeout。该函数也包含完整版超时恢复相关的进程操作；本轮没有执行任何分支。

## 无现成窗口：临时文件＋命令行

当最终找不到窗口，10005216调用10004e8d。Mini分支：

1. 定位安装目录并拼接ThunderMini.exe，检查路径。
2. GetTempPathW/GetTempFileNameW产生文件，CreateFileW后WriteFile把刚才UTF-16任务包保存到临时文件。
3. 用` /%s:%s`和键`MiniXlAgentFile`拼接参数，ShellExecuteW打开ThunderMini.exe。

等价于：`ThunderMini.exe /MiniXlAgentFile:<temp-path>`。这不是JSON或纯URL文本，而是前述二进制任务包。完整版另有` /AgentFile:`和`-StartType:BHO_Context`，不能混成Mini命令。

10005216在启动文件路径后把业务结果设置为1；这只证明程序启动/请求移交路径，未证明引擎已经创建下载任务，也没有证明临时文件最终删除时机。接收端必须继续对照ThunderMini.exe的消息0x2410和MiniXlAgentFile解析链。

## Firefox接口版本缺口已闭合

对Firefox DLL原始GUID数据按PE节映射读取（VA去image base，再按节换文件偏移，不能直接用RVA当文件偏移）：

| Firefox VA | GUID | TYPELIB |
|---|---|---|
|10004e2c|1622f56a-0c55-464c-b472-377845def21d|IAgent|
|10004e3c|152bb689-43e0-4167-b557-cdc0e57c03ea|IAgent9|
|10004ef0|d3830c5b-62ea-48ef-a7cb-5b3944cae12f|IAgent4|
|10004f0c|e6edfd5a-86f5-4af6-9fab-0a30be6c4b7f|IAgent10|

因此Firefox 10001e90先创建IAgent，再QueryInterface为IAgent10；10001d30的+0x44明确为AddTask4，100018d0的+0x64明确为CommitTasks5，100018a0的+0x28为CancelTasks。前份browser-native-bridge.md中的接口命名缺口由此解决。

## 结论及剩余边界

浏览器插件本身不实现文件下载。它们把任务交给COM代理；代理先入内存队列，随后以共享映射＋窗口消息，或临时任务包＋启动参数交给下载主程序。Mini分支的任务移交和迅雷资源加速网络没有必然关系，现代新产品可以借鉴该分层，使用自己的结构化本机通信和明确接收确认。

仍待主程序侧验证：消息参数如何校验、任务包字段如何转引擎调用、业务结果具体含义、重复提交和临时文件清理。静态结论没有替代实际浏览器与下载实测。

## 离线格式验证工具（2026-10-18）

`scripts/minithunder-agent-packet.mjs` 是独立只读、有界的格式解释工具，命令：

`node scripts/minithunder-agent-packet.mjs <packet-path>`

JSON保留六个头部DWORD、字段顺序、重复字段、偏移、未知UTF-16字段、数值文本和TaskOption的有符号/无符号值；二进制保存base64。Cookie及名称含authorization/password/token的字段默认脱敏；只在显式`--include-secrets`时显示。它不会解析/运行原版DLL，不依赖旧解析器。限制为64MiB包、1000任务、每任务128字段、文本1MiB、二进制8MiB；这些是工具防护界限，**不是原版协议最大值**。零任务与未知头部数值保留；不推测头部语义。未知字段按原接收器fallback解释成长度前缀UTF-16，不支持未知二进制类型。严格拒绝尾随字节，便于发现包边界误判。

`analysis/tests/minithunder-agent-packet.test.mjs` 有7项合成测试（TAP结果同目录）：合法17字段、Unicode、负数数值、Cookie脱敏/显式展示、二进制保持、每一个截断位置、计数负数的DWORD表示/大计数、超长字段与包、奇数UTF-16名称及值长度、未知字段、尾随数据、空任务。通过只证明工具在该格式理解下处理合成输入；**不证明捕获了真实包、不代表原版动态验收通过**。

### 对照主程序接收器

依据`analysis/exports/minithunder-ghidra/pseudocode/0044c012.c`，下列字段明确进入恢复的临时任务结构：URL、FileName、ReferURL、Cookie、CID、Path；ProtocolInfo/RecvData分别读入二进制缓冲区，TaskOption读取原始DWORD。还识别OpenDirect、Update2XL并与T比较，这两项不在代理常规17字段名单中。

代理发送但0044c012未见赋值/分支消费的字段：Comments、StartMode、OnlyFromOrigin、OriginThreadCount、StatURL、OriginExplorer、StatClick、CharSet。它们会按普通文本读过然后丢弃。该判断只覆盖当前恢复的接收函数，不能外推为整个引擎不具备相关能力；例如主程序其他入口可以另设线程数和Cookie。

`0042d1a0.c`核实文件入口打开并映射任务文件、调用0044c012、交给后续提交函数；在成功打开路径中随后检查并DeleteFileW临时文件。上文“临时文件删除时机未证明”的缺口由此缩小：已确认该文件入口的删除调用，尚无真实运行证明失败/异常所有路径都清理。

旧接收器直接移动指针/信任包中计数，ProtocolInfo只有很弱的长度下界，RecvData使用约50MiB上界；本工具不复制这些不受控读取逻辑。新工具的字段`consumedByRecoveredParser`只表示0044c012中有直接读入/赋值证据，不表示该字段最终被下载引擎有效使用。

## 浏览器预读数据到创建入口及引擎命令（补充）

补充产物：`analysis/exports/minithunder-browser-receiver/`，`minithunder-context-refs.txt`，`minithunder-browser-vtable.txt`。主程序Ghidra数据库本轮独占读取，未执行样本。

### 接收任务结构与创建链

0044c012 每任务建立0x44字节临时记录。ProtocolInfo使用记录+0x20处的缓冲对象，其数据起点/终点为+0x24/+0x28；RecvData使用+0x30处缓冲对象，起点/终点为+0x34/+0x38；TaskOption为+0x40。这些偏移可由解析器局部记录基址EBP-0x70和二进制对象EBP-0x50/EBP-0x40相减得到，并被0041dffc的取值互证。0041da5c把记录追加至vector（0041d774按0x44算索引），随后0044c012销毁原临时记录；不能把短命栈地址当成任务长期所有者。

0041e384是提交入口，正常分支到0041dffc。后者从记录+0x34/+0x38取得RecvData地址和长度；存在非空预读数据时，另从+0x24/+0x28取ProtocolInfo（否则相关指针置零），经0041b518弹出任务确认界面，再到0041a68b创建。批量后续任务也由0041a68b创建。该流程意味着“包有ProtocolInfo”不必然就会复用连接：此分支还依赖预读长度及用户/任务流程。

0041a68b普通下载分支通过任务管理对象虚表+0xa4传URL、文件名、目录、引用页、Cookie等，以及最后三项ProtocolInfo、RecvData、RecvData长度，输出任务接口。原始指针表中地址004a4b7c为0044d49a；表基址004a4ad8与之相差0xa4，构造器00450a9e/00450bcf引用该表。静态虚表布局与调用点吻合，闭合间接调用，不依赖函数索引缺失的直呼边。

0044d49a→0046050b→00460236：创建任务对象；后者在ProtocolInfo非空时WSASocketW重建socket，保存到任务+0x2c8，再WSADuplicateSocketW生成当前进程协议块到+0x2dc。RecvData复制到任务缓冲对象+0x2cc（实际起点+0x2d0、终点+0x2d4）。这一步是“接收/准备复用”，还不是引擎已经开始从该连接下载的动态证明。

### 附加信息命令：主程序到引擎参数已确认

0045dcc2调用0045d187。后者在任务+0x2dc存在时调用：

`0045cb9a(taskId, protocolInfoPtr, recvDataPtr, recvDataLength, &command)`

taskId来源任务+0x2c，协议块任务+0x2dc，数据任务+0x2d0，长度为+0x2d4减+0x2d0。0045cb9a分配命令对象并调004766d2；该构造函数把0x274字节协议块复制到command+0x1c，预读数据复制到command+0x20，长度到command+0x24。数据分配len+1并补NUL；传输仍保留显式长度。

0045d187取任务+0x18c接口的虚表+0x28返回对象，再00461155入队。对应执行wrapper00476af8位于指针表004a580c；其指令00476af8–00476b13取命令+0x1c/+0x20/+0x24并调用引擎槽004c9bc0。准确参数顺序为：

`set_addition_info(taskId, protocolInfoPtr, recvDataPtr, recvDataLength)`

00476b08后把引擎返回0转换为true。该结果只表示接口调用约定，仍不能单靠wrapper布尔证明引擎成功接受上下文；引擎导出内部的错误传播需结合独立引擎报告。

### 旧IE `_infow` 文件与Mini路径必须区分

当前已证实Mini接收的是`MiniXlAgentFile`命令键和`MiniXl.Thunder.Agent..._TaskData_线程ID`共享映射，二者进入同一Unicode任务列表解析器0044c012。主程序strings导出及当前函数引用索引未发现字面`_infow`的专用解析入口；0042d1a0接受的临时文件内容为前述任务包，并有映射/解析/删除路径。

因此本轮**没有证明旧IE `_infow` 文件路径可被Mini直接读取或等价处理**。不能因为共享包中存在ProtocolInfo/RecvData、Mini支持临时文件参数，就把它归并成`_infow`格式支持。也不能仅凭没有字面字符串排除动态构造或其他DLL路径；若要证明这一点，必须拿到BHO生成该文件的准确命令/参数与接收端解析函数一一对应。现阶段可确认的是Mini的COM任务包和socket上下文通道，旧IE `_infow`兼容性保持未证实。
