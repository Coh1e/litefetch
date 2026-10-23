# 浏览器本机桥接：函数级静态证据

2026-10-18。补充 minithunder-state-resources.md。未运行样本或注册组件。Ghidra 元数据、完整函数索引、选定伪代码、单函数 .c 文件分别位于 analysis/exports/browser-chrome-ghidra 和 browser-firefox-ghidra。metadata.json 记录被分析 DLL SHA-256、image base 与语言。下文地址是装载 VA，不能直接当文件偏移。

## Chrome NPAPI → COM 已证实

1. JS background.html 调 embed.DownLoadByThunder(url)，见前份报告。
2. NPAPI 相关构造函数 10001b00 引用 DownLoadByThunder 并初始化 identifier DAT_1000edd0。100029c0 比较传入 identifier 与该全局，要求参数数目为 1，将结果 NPVariant 初始化为布尔 false。
3. 100029c0 调 100024f0，使用 MultiByteToWideChar(65001) 把 UTF-8 参数转 UTF-16；继而调 10002590。成功非零时将返回布尔设 true。
4. 10002590 调 CLSIDFromProgID(L"ThunderAgent.Agent")、CoInitialize、CoCreateInstance(CLSCTX=1)。通过 IDispatch vtable +0x14 分别解析 AddTask4、CommitTasks4，再通过 +0x18 Invoke。
5. AddTask4 的 DISPPARAMS.cArgs=11；VARIANT 数组中有传入 URL、多个空字符串和 -1/0 等默认值。按 COM 参数反序解释，URL 位于首个逻辑参数。此 DLL 没有自行读取页面 Cookie/Referer 的证据；从 JS 到 DLL 的已证实输入只有 URL。
6. AddTask4 Invoke HRESULT<0 则清理后 return 0；解析 ProgID、创建对象、解析方法名失败也 return 0。成功后 Invoke CommitTasks4，cArgs=2，反序数组值为 1、0，即逻辑顺序 0、1；返回其 VARIANT payload（Ghidra 标为 uStack_44）。这里未独立检查 Commit Invoke HRESULT。
7. NPAPI 返回 false 对应 background.html 的 chrome.tabs.create({url:req.link})。这证明失败回退路径存在，但没有运行验证“回退是否再次触发拦截”等浏览器行为。

函数索引显示 100029c0 直接调用 10002590。NPAPI callback 表及 identifier 初始化尚需完整数据引用复核，不能把构造函数的部分异常栈推断当精确 C++ 源码。也没有证据显示 DLL直接把请求交给 ThunderMini.exe：它到达的是注册 ProgID，最终对象由安装注册决定。

## Firefox ctypes → COM 已证实与限制

CallThunder 导出 100011a0 包装两份 UTF-16 字符串并调用 100021c0。后者依据 async 字符串是否为 L"1" 分流并调用 10001e90。JS传入的 monitor 参数在当前伪代码中没有明显继续消费，需要原指令核对。

10001e90 原始指令位于 bridge-listing.txt。初始伪代码把 QueryInterface 错误恢复为无参数函数，继而把后续有效代码标记为不可达；因此不能使用“伪代码 return false”宣称整个桥接不可用。

原始指令可确认：

- 10001ecc 引用 ThunderAgent.Agent，随后 CLSIDFromProgID 与 CoCreateInstance。
- 10001f09–10001f1a 为 QueryInterface：推送结果指针、IID指针10004f0c、this，再调用 vtable[0]；检查 HRESULT 和结果指针。
- 10001f40、10001f55 及循环内多次调用 10001910。此函数用 wcsstr 找 `#@$@#`，拷贝字段并前移输入游标。
- 第一项是来源页，第二项转为计数；循环读取 URL、链接描述、Cookie。Cookie 字符串为 `null` 时替换为空串（需结合 JS与DLL字符串复核）。
- 10002130 调 10001d30；该 helper 调目标 COM vtable +0x44。它携带 URL、空保存路径/文件名、描述、来源页、默认 -1/0/-1、Cookie及空附加字段一组参数；Ghidra推断函数参数不完整，精确签名必须借类型库恢复。
- 1000217e/1000218b 调 100018d0，helper 调 COM vtable +0x64；10002194 可调 100018a0，后者对应另一个 COM vtable +0x28。前者有两枚栈参数，分支是 1,1 或 1,0 的调用约定顺序；尚不擅自命名 CommitTasksN。
- 返回阶段 100021ad–100021b2 对 ESI==1 做布尔化，Release 接口后回传；CallThunder 外层用输出指针把此结果给 JS，JS用该值决定是否 return false 取消默认导航。

**未证明**：Firefox COM IID 对应的确切接口版本、vtable +0x44/+0x64/+0x28 的官方方法名、所有参数的类型和意义。应对照 ThunderAgent.dll typelib 或主程序 COM 接口恢复，而不是由 Chrome 的方法名类推。

## 新产品可借鉴点

提取到的实质机制是“浏览器先识别用户下载意图→把请求交给本机→本机明确反馈是否接收→浏览器决定取消或回退”。Chrome旧插件过早 preventDefault且只传URL；Firefox传上下文更完整但固定分隔符和固定缓冲区不适合复用。新版应传结构化请求并先确认入队成功，再停止原下载；其现代浏览器API实现仍按第二阶段执行，不复制 NPAPI/XUL。

后续更新：Firefox接口版本和方法名已由ThunderAgent嵌入类型库闭合，见browser-agent-ipc.md。+0x44为AddTask4，+0x64为CommitTasks5，+0x28为CancelTasks。
