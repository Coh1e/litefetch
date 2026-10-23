# Minithunder 引擎 ABI 与协议边界：静态证据

日期：2026-10-18。范围：原样读取解包文件，不运行样本、不连接迅雷网络。此报告是 PE/字符串层证据，未恢复参数结构体、调用约定或函数内部语义。

## 可复现产物

在仓库根目录执行 `node scripts/minithunder-engine-pe.mjs`，产物位于 `analysis/exports/minithunder-engine-abi/`。脚本解析 PE32/PE32+ 导出、导入、节区及 ASCII/ASCII 范围 UTF-16LE 字符串；字符串偏移为文件偏移，导出地址为 RVA，二者不可混用。当前四个样本均为 x86，主程序 ImageBase 0x400000，DLL ImageBase 0x10000000。

| 文件 | SHA-256 | 命名导出 | 导入函数 |
|---|---|---:|---:|
| download_engine.dll | 38dd7f676a592bf1810e314e370b28fa4a43e2df3e6a8181447335333d1b0083 | 214 | 503 |
| dl_lib.dll | 19f38faf9eeba0cd085aba8c06947c536832d0e2abc704a33e42821048c4e2a2 | 15 | 163 |
| dphubt.dll | 0117a43562b51a248645046720b23a1ed780d755c5be7d42d09d52ad74d1d32f | 4 | 154 |
| ThunderMini.exe | 8f48711dee8a2dab143567a9afc8923ca94465e98f6a9c609e59a0012d8d85af | 0 | 732 |

## 主程序与引擎连接

主程序的静态导入表 **没有 download_engine.dll、dl_lib.dll 或 dphubt.dll**。因此不可称为“主程序静态导入了这些下载接口”。主程序导入了 LoadLibraryA/W、LoadLibraryExA/W、GetProcAddress，并包含 81 个与引擎导出精确同名的字符串，集中于 0xa4fac—0xa55e8。它们是后续追踪动态解析表的直接入口，但字符串本身不证明对应函数被实际调用。

完整匹配见 `main-engine-symbol-strings.json`；空的 `main-engine-imports.json` 记录静态导入检查结果。

| API | 主程序字符串文件偏移 | 引擎导出 RVA | 证据解释 |
|---|---|---|---|
| create_new_task | 0xa55c0 | 0x943f | 普通任务入口名称配对 |
| create_continued_task | 0xa55a8 | 0x9617 | 恢复任务入口名称配对 |
| start_task / stop_task | 0xa55dc / 0xa55d0 | 0x640c / 0x64b0 | 生命周期入口名称配对 |
| query_task_info / delete_task | 0xa558c / 0xa559c | 0xa8be / 0x6e80 | 查询与删除入口名称配对 |
| create_new_bt_task | 0xa5334 | 0x1855 | BT 创建入口名称配对 |
| create_continued_bt_task | 0xa5318 | 0x18f2 | BT 恢复入口名称配对 |
| create_new_emule_task2 | 0xa516c | 0x7518 | eMule 创建入口名称配对 |
| create_continued_emule_task2 | 0xa5184 | 0x7717 | eMule 恢复入口名称配对 |
| set_ed2k_enable / set_kad_enable | 0xa51e0 / 0xa51f0 | 0x90d4 / 0x9143 | 独立协议开关名称配对 |
| set_thread_num / set_retry_strategy | 0xa54c4 / 0xa54b0 | 0xad17 / 0x66a6 | 线程/重试设置名称配对 |
| set_res_use_strategy | 0xa54d4 | 0xabae | 资源使用策略入口；位定义尚未知 |

## 协议能力边界

### 普通下载

引擎导出 create_new_task、create_continued_task、create_predownload_task、set_thread_num、set_retry_strategy、set_cookie、set_http_request_header、set_http_request_method、set_post_data、set_last_modified、set_task_download_speed_limit、get_failure_detail。主程序有其中部分名称，例如 Cookie、线程数、重试和恢复；没有名称并不排除通过其他入口使用。

HTTP Range 解析还存在额外静态证据：文件偏移 0x37a320 是 `Content-Range`，0x37a39c—0x37a4cc 包含无效或缺失 Content-Range 的错误文本。这能证明解析相关代码/数据存在，不能证明续传时正确检查 ETag、正确处理服务器忽略 Range，或具有自适应分段算法。

### BT 与磁力链

BT 导出包含 parse_seed_file、create_new_bt_task、create_continued_bt_task、create_upload_bt_task、reset_bt_task_need_download_file、set_bt_file_download_order、query_bt_task_info、get_bt_peer_info_list，以及 BT 文件重新导入接口。前述创建/恢复/文件选择与顺序等名称出现在主程序。

磁力链单独导出 parse_magnet_url（RVA 0x241e）、create_magnet_task（0x2510）、get_magnet_task_status（0x25d0）。**三者没有在主程序 ASCII/ASCII 范围 UTF-16 字符串中精确匹配。** 因而“引擎包含磁力链接口”证据强于“精简版 GUI 已接入磁力链”；后者仍需主程序分流和调用链证据。

### ed2k 与 Kad

除创建/恢复接口外，存在 parse_ed2k_url、create_ed2k_link、get_emule_task_info、set_source_upmost、set_sametime_connecting_peers、set_emule_crypt_option、set_secureident_option、set_emule_kad_port，以及服务器列表、连接与优先级 API。主程序有创建 task2、恢复 task2、ed2k/Kad 开关、加密/身份、Kad 端口等名称。

引擎数据中可见 CEmuleInterface（0x3934cc）、CEmuleSubTask（0x393bb0）、CEmuleTask（0x393d38）及相应 RTTI 名称；这比单一导出名称多一层组件存在证据，仍不是联网成功证明。

### 迅雷网络：新产品明确排除

原引擎将普通/P2P 任务和专有资源体系混合在同一 DLL 中。可疑专有边界包括 accelerate_task、can_accelerate_task、add_peer_resource、add_server_resource、query_resource_info、get_task_gcid、set_res_query_cid、report_file_to_phub、set_hub_proxy、thunderS_register_client、set_thunderS_pingInfo、set_thunder_peer_extern_credit 等。**这些名称只能用来划分审查范围，尚不能逐项断言全部只服务于迅雷网络。**

额外字符串：p2sp_enable（0x370214）、p2sp_speedup_enable（0x370234）、CP2spDownload（0x378084）、p2sp.dll（0x3735b0）、p2sp_pd.dll（0x373c58）。BT 还含 get_bt_p2sp_sub_task_file_indexs，ed2k 同时暴露 CID/GCID，提示旧实现存在协议与专有资源元数据的交叉。不能靠关闭一个开关就声称旧 DLL 已成为纯标准协议核心。

新产品结论：不分发或加载原下载 DLL；标准 HTTP/BT/ed2k 由独立开源实现提供。仅借鉴原程序经证据确认的流程与状态。迅雷服务注册、资源发现、信用、汇报、专有 CID/GCID 查源均不进入新版需求。

## 两个辅助 DLL

dl_lib.dll 的 15 个命名导出集中于 URL/ed2k/种子解析、临时文件存在检查、BT 文件列表读取、格式化 URL，与引擎部分接口重叠。当前证据支持“辅助解析/本地文件能力库”的描述，不支持认定其承担传输。

dphubt.dll 仅导出四个 COM 标准入口 DllCanUnloadNow、DllGetClassObject、DllRegisterServer、DllUnregisterServer，没有命名下载任务接口。其内部角色与 COM GUID/vtable 需要另行恢复；不能从文件名推断它是必要 BT 引擎。

## 明确缺口

1. 主程序 LoadLibrary/GetProcAddress 解析函数及各名称对应的函数指针槽位、错误处理和是否必需。
2. 各导出参数布局、返回码、线程/回调与对象所有权；PE 导出无法提供函数原型。
3. 普通/BT/ed2k 的 GUI 分流条件、启用配置和失败回退；磁力链是否由其他路径或进程承接。
4. 停止与暂停是否不同、恢复数据文件格式、BT 文件索引/优先级的编码。
5. 真正连接和传输的动态行为；本报告未运行原程序，未连接任何网络。

这些缺口应由有针对性的调用链反编译与隔离测试补足，不应把本轮静态接口盘点冒充完整逆向。
