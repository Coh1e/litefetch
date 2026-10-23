# Minithunder 1.5.3.288：浏览器接管、界面与任务持久化静态证据

日期：2026-10-18。仅解包、文本阅读和字节扫描；没有启动原版、注册组件、加载扩展或连接迅雷网络。

## 证据范围

主程序 `work/minithunder-unpacked/Bin/ThunderMini.exe` SHA-256：`8F48711DEE8A2DAB143567A9AFC8923CA94465E98F6A9C609E59A0012D8D85AF`。其他输入哈希见 `analysis/exports/minithunder-state/sources.json`。数据库/状态字符串的原始文件偏移、编码、已有 strings 导出行号见 `strings-offsets.tsv`，不是运行轨迹。复现脚本：`analysis/scripts/export-minithunder-state.ps1`。

以下浏览器源码路径均相对 `analysis/exports/minithunder-state/browser/`。原始 CRX/XPI 由 7-Zip 解包，Firefox `chrome/thunder.jar` 再解包到 `thunder-jar/`。这些解包文件保留原始内容及行号。

## 浏览器自动接管：已确认的 JavaScript 路径

### Chrome

- `xl_plugin_chrome.crx/manifest.json:15–21` 在 HTTP/HTTPS 页面注入 `js/xl.js`；23–26 注册 `xl_chrome.dll` 原生插件。权限仅有 contextMenus、tabs、HTTP/HTTPS；该 manifest 没有下载事件或请求拦截权限。
- `js/xl.js:63–92` 定义资源扩展名白名单及协议正则：rar/zip/tar/iso/exe/dll、音视频、chm/mdf/torrent；HTTP/FTP/HTTPS 按后缀，另识别 thunder/ed2k。此名单未含 magnet。
- `js/xl.js:244–314` 的 `PreParseURL` 附带新浪、太平洋、中关村、非凡、天空、gougou 等旧站点规则；其中 `if( bValidate = true )`（赋值而非比较）会无条件通过对应分支。应记录为源代码问题，不照搬。
- `js/xl.js:349–353` 对注入当时的 `document.links` 逐个绑定 click。`onLinkClick`（220–233）判断 URL，通过后 `chrome.extension.sendRequest({name:'xl',link:theURL})`，并立即 `e.preventDefault()` 取消该次链接默认导航。
- `background.html:8–17` 接到请求调用 `InvokeThunder(req.link)`，返回 false 则新建标签打开原链接；25–29 取得 NPAPI embed 并调用 `DownLoadByThunder(url)`。21–24 另外提供链接右键菜单。
- Chrome 这层只明确传递目标 URL；未见 Cookie、Referer、请求方法、POST body 或 response header 被此 JS 传出。
- `_5.crx/background.html:8–24` 同样桥接；该变体没有标准 contextMenus 注册。两份 DLL 应分别逆向，不能仅由相同接口名推定完全相同实现。

结论：这份 Chrome 插件可由源码确认的是**页面点击接管**，在浏览器真正发起原导航前拦住链接；不能称为“监听所有下载响应并取消浏览器下载”。单页应用后加链接、表单 POST、JS 生成下载、无后缀响应及响应后文件名识别不在这条已证实路径内。

### Firefox

- `thunder.xpi/chrome.manifest:5` 把 overlay 装入浏览器 XUL；`thunder-jar/content/main.xul:8–14` 引入脚本及右键下载入口。
- `content/download.js:6–18` 的 `isHookDownload` 调 `LoadProfile` 后设置固定后缀并直接 return true；后续 MonitorIE/ExtendNames 配置读取不可达。不能声称此版本按配置关闭拦截。
- `download.js:78–151` 的 `checkUrl` 只接受左键和 A 元素/其直接父元素；支持 thunderHref 属性、含 ? 链接交 DLL 的 `WhetherDynamicLink`、其余按后缀识别。调用返回 0 则 return true；成功则 return false。
- `download.js:158–163` 设置 `window.document.onclick=checkUrl`，通过事件返回值阻止默认行为。这里也不是观察 HTTP 响应。
- `download.js:46–59`、61–76 拼接页面 URL、数量 1、下载 URL、链接文本（去 NUL）、目标 URI Cookie；分隔符定义于 `public.js:24` 为 `#@$@#`。`public.js:158–169` 包装本机调用。
- `product.js:18–37` 从 Firefox profile extensions 目录用 `ctypes.open` 加载 ThunderComponent.dll，声明 CallThunder/WhetherDynamicLink 等导出；54–57 传 UTF-16 字符串、async/monitor/all 标记及结果指针。
- `main.js:42–74` 右键单项路径也传 Cookie；77–136 枚举链接/图片进行批量提交。

结论：Firefox 的已证实路径传输上下文比 Chrome JS 更完整，但 DLL 后面如何调用 COM/发送消息仍需函数级证据；不能由脚本断言最后落在哪个 EXE。

### IE 右键资源与本机 DLL 边界

`work/minithunder-unpacked/bho/minixlgeturl.htm:4–30` 明确创建 `ThunderAgent.Agent`，调用 `AddTask5` 及 `CommitTasks5(1,1)`；传 Url、Info、Location、Cookie、CID、StatUrl 等。页面可通过 thunder_down_pageurl/thunder_stat_pageurl/thunder_cid 元素覆盖相应字段（48–71）。`minixlgetAllurl.htm` 枚举 document.links/images 并批量 AddTask5。此为右键动作脚本，不能用它证明 BHO 自动下载监控机制。

本机字符串扫描分别保存为 `xl_chrome.dll.strings.tsv`、`ThunderComponent.dll.strings.tsv`、`XunleiBHO.dll.strings.tsv`。例如 Chrome DLL 含 DownLoadByThunder（0xB474）、CommitTasks4（UTF-16，0xB494）、AddTask4（0xB4B0）、ThunderAgent.Agent（0xB4C4）、CoCreateInstance；这些支持 COM 桥接假设，但**还不是已核实的调用链**。IE BHO 网络拦截、取消下载与状态回退必须继续反汇编确认。

## 界面状态与动作

`Skin/downloader.xml:132–146` 给出六个图标位置：started=0、stoped=1、failed=2、waiting=3、success=4、open=5。open 是资源/动作名称，不能当成下载协议状态。74–83 提供成功、进行、失败与进度背景；115–152 有 open/stop/start/restart/opendir/delete 按钮。

strings 导出 5676–5682 有 TS_Failed、TS_Succeeded、TS_Stopped、TS_Started、TS_StartWaiting、TS_StandBy、TS_Unknown。它们是候选逻辑状态名；字符串排列、图标索引都**不能证明枚举数值或状态转换**。xml:197–212 及 strings:7255–7288 提供重新下载、开始、停止、删除、打开文件/目录、BT 种子另存为菜单。

删除提示 strings:5229–5235、7301–7303 区分删除任务与同时删除文件，并有“任务即使运行也删除”。已有 delete_task/stop_task/start_task/create_continued_task 名称，但删除前停止顺序、引擎回调后的状态写入、暂停是否 flush 仍待函数链核实。

## 任务数据库结构

strings:5767 名称为 TaskDb.dat。5683–5742 含带占位符的 SQL 模板（`$:Table$` 并非最终 SQL 表名），显示共享 TaskBase 加协议附表的设计：

| 表模板 | 源码字符串字段证据 | 位置 |
|---|---|---|
| TaskBase | TaskId, Type, Status, StatusChangeTime, SavePath, TotalReceiveSize, TotalSendSize, TotalReceiveValidSize, TotalUploadSize, CreationTime, FileCreated, CompletionTime, DownloadingPeriod, RemovingToRecycleTime, FailureErrorCode, Url, ReferenceUrl, ResourceSize, Name, Cid, Gcid, Description, ResourceQueryCid, NamingType, StatisticsReferenceUrl, UserRead, SafeFlag, BackGround, OpenOnComplete, Proxy, UserData | SELECT 5736；INSERT 5741–5742 |
| P2spTask | TaskId, ResourceUsageStrategy, ResourceReportStrategy, Cookie, AccountNeeded, UserName, Password, OriginResourceSupportRange, OriginResourceThreadCount, FileNameFixed, RetryTime, HttpRequestHeader | SELECT 5735；INSERT 5739–5740 |
| BtTask | TaskId, SeedFile, InfoId, DownloadFileOrderData, Tracker, DisplayName | 5702、5734 |
| BtFile | BtFileId, BtTaskId, Status, FileIndex, Download, FilePath, FileName, FileSize, FileOffset, ReceivedSize, Cid, Gcid | 5701、5732 |
| EmuleTask | TaskId, FileHash, ConfigureFilePath | 5684、5733 |

Status 与 StatusChangeTime 共同更新（5717）；完成时间、错误码、进度统计各有独立 UPDATE（5709–5713）。5741 的插入模板带 `WHERE NOT EXISTS ... WHERE Url=?30`，证明至少存在按 URL 避免重复插入的一种模板；不能断言所有入口去重、事务原子性或不同保存目录处理。表创建使用动态拼接（5780 的 CREATE TABLE），尚无真实数据库，不能确定完整 DDL、索引、约束、枚举值与字段类型。`RetryTime` 的 TaskBase UPDATE（5706）与 P2spTask SELECT 差异也需核对运行 schema。

这些字段揭示新版可借鉴的“共享任务＋协议恢复数据”分层；Cid/Gcid、资源上报/查询及迅雷代理字段不应直接复制到新产品。

## 批量下载与协议提示

strings:5286–5314 给出单个 `(*)` 占位符规则、数值范围及零填充长度；例 `http://www.a.com/(*).zip`、1–10、宽度 2/3。资源文本还给宽度提示 (1-3)（7311）。“URL中只能包含一个/必须包含一个”与 `%%0.%dd` 支持单通配符格式化解释；范围逆序、负数、溢出上限等未得到验证。

strings:5339–5351 出现 ftps/https/mmst/mms/rtspt/rtsp/magnet/minithunder/thunder/ed2k/ftp/http。6901 等重复资源包含“精简版不支持%s类型下载 <%s>”。这些仅证明前端有候选前缀与拒绝提示，不等于全部协议可用；实际分流与拒绝集合等待主程序调用链。

## 下一步尚需证明

1. BHO 下载监控的接口/事件入口、识别条件、取消原下载条件及确认失败后的回退。
2. Chrome NPAPI 与 Firefox DLL 到 ThunderAgent、主程序的准确方法参数及 IPC。
3. 主程序任务入库顺序、引擎恢复数据生命周期、状态枚举数值与转换。
4. 真实数据库结构及批量 URL 的边界行为。

新版浏览器扩展属于已同意的第二阶段。当前逆向结论说明它应使用现代浏览器明确支持的下载事件与本机通信，支持可撤销接管和上下文传递，不能照搬这套旧 NPAPI/XUL/COM 页面脚本。此处是设计建议，未在本轮验证现代 API。

