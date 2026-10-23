# Minithunder：引擎动态绑定与六个创建/恢复包装

日期：2026-10-18。输入为 `analysis/exports/minithunder-ghidra/{selected.jsonl,functions.jsonl,api-slot-references.jsonl,pseudocode/}`，并用 PE 文件校验 `init` 名称。不启动原程序、不连接迅雷网络。

## 证据强度与记号

本轮已由“同名字符串存在”推进到 **加载表、间接调用槽位、实际包装函数体** 的静态对应。仍没有引擎内部函数反编译和运行验证，不能将包装伪代码写成可编译 ABI 头文件。

多个函数被反编译为 `(void)` 并出现 `unaff_EBP`，说明 Ghidra 未恢复共享 SEH 序言后的参数。下文 `S08/S0c/...` 仅表示伪代码读取的 `[EBP+0x08]/[EBP+0x0c]/...`。`A(x)` 表示经 CW2AEX 转换后的窄字符串，`L(x)=strlen(A(x))`，长度为字节且不含结尾 NUL。**尚未证明转换使用 UTF-8**，不得给新版照搬代码页行为。

## 动态加载：00460931

`FUN_00460931` 检查模块变量 `DAT_004c9c50`，从程序路径派生库路径，经 PathFileExistsW、LoadLibraryW 加载；随后逐条执行 GetProcAddress 并填全局槽位。循环长度 `0x42=66`。任一符号缺失即 FreeLibrary、将模块变量归零并退出失败；全部成功后调用辅助加载器 `00460677`，返回 1。模块原本已非空时返回 1。

限制：DLL 路径拼接经 `0042e157`，当前选取函数没有恢复出该函数使用的字符串；因此仅由当前伪代码不能精确写出绝对加载路径。辅助加载器的返回值没有在 00460931 内被检测，不能将 00460931 成功等同于两个库都成功。

`00460677` 使用另一模块变量 `DAT_004c9c90` 和 15 条解析/临时文件函数表，逐项 GetProcAddress，缺失卸载；与 dl_lib.dll 的 15 导出集合吻合。66 项主表与 download_engine.dll 导出吻合。二者加载路径文件名仍需拼接函数交叉验证。

### 本轮主要槽位

| 全局槽位 | 动态解析名称 | 已见包装/调用 |
|---|---|---|
| 004c9b28 | init | 0045643b |
| 004c9b2c | uninit | 加载表确认 |
| 004c9b30 / 004c9b34 | start_task / stop_task | 六个创建包装没有调用 |
| 004c9b38 / 004c9b3c | create_new_task / create_continued_task | 0047863a / 004788ad |
| 004c9bcc / 004c9bd0 | create_new_bt_task / create_continued_bt_task | 00478b09 / 00478c74 |
| 004c9c30 / 004c9c34 | create_continued_emule_task2 / create_new_emule_task2 | **00478f4a / 0047917f** |
| 004c9bac | set_cookie | 00476e2f |
| 004c9bb4 / 004c9bb0 | set_file_name / set_complete_file_name | 00476ec6 / 00476f5d |
| 004c9be4 / 004c9c38 | set_bt_task_stat_para / set_emule_task_stat_para | 0047713a / 00477286 |
| 004c9bd8 | notify_hub_bt_moved | 00476ff4 |

特别注意：ed2k 两个函数按地址排列是“恢复在前，新建在后”，不可按普通/BT 地址顺序类推。主加载表没有磁力链三接口，也没有 HTTP request-header/method/post-data 接口；这比“主程序缺少对应字符串”更明确，但不能排除其它装载路径。

初始名称 `&DAT_004a63f0` 已从样本字节验证为 `init\0`（文件偏移 0xa55f0），不是未经识别的第四种任务 API。

## 普通任务包装

### 新建 0047863a → create_new_task

观察到 S08、S0c、S10、S14、S18 转窄字符串，S1c 作为不透明值保存。S18 为 NULL 时先替换为空宽字符串；其转换结果若为 NULL 还有局部空字节 fallback。A(S08) 或 A(S14) 为 NULL 时本地返回 `0xffffffff`；S0c/S10 可按 NULL 指针及零长度传入。

按伪代码显示的调用参数顺序：

```text
create_new_task(
  A(S08), L(S08), A(S0c), L_or_zero(S0c),
  A(S10), L_or_zero(S10), A(S14), L(S14),
  A(S18_or_empty), L(S18_or_empty), S1c)
```

包装在引擎返回后释放转换用内存并原样返回结果，没有调用 start_task。S1c 是否输出 task ID、回调或其他结构，当前证据没有类型说明；必须从调用者及引擎内部确定。

### 恢复 004788ad → create_continued_task

同样转换五个宽字符串，但 A(S10) 转换后不进入可见引擎调用。A(S08) 与 A(S14) 是必需转换指针；S0c/S18 根据原始宽指针是否 NULL 选择传入值或 NULL+0。可见调用：

```text
create_continued_task(
  A(S08), L(S08), optional_A(S0c), optional_L(S0c),
  0, 0, A(S14), L(S14), optional_A(S18), optional_L(S18), S1c)
```

“第三组被显式置零”是当前确定的创建/恢复差异；不能仅凭位置将第三组命名为 Referer、Cookie、文件名或临时文件路径。

## BT 包装

00478b09、00478c74 具有相同预处理结构，分别调用 create_new_bt_task 与 create_continued_bt_task。

1. 转换 S08、S0c；任一转换指针为 NULL，本地返回 `0xffffffff`。
2. 申请 `S18 * 0x404` 字节的临时缓冲。
3. 调用 `00478226(S1c, S18, buffer, &convertedCount)`；输出计数先置零。
4. 可见参数顺序：

```text
create_{new|continued}_bt_task(
  A(S08), L(S08), A(S0c), L(S0c),
  S10, S14, convertedCount, buffer, S20)
```

5. 返回前释放 buffer 和两个转换字符串，结果原样返回。

补导 `00478226` 后已确认条目转换：输入是宽字符串指针数组，遍历 S18 项，仅跳过 NULL 项；每个非 NULL 项调用 `WideCharToMultiByte(0,0,text,wcslen(text),entry+4,0x400,NULL,NULL)`。**代码页 0 是系统 ANSI 代码页，不是 UTF-8**。条目前四字节写转换函数返回的字节数，随后是最多 0x400 字节数据，步长 0x404；输入不含 NUL 长度，也没有显式写结尾 NUL。

转换返回 0 时 helper 仍累加输出条目数；因此不能断言转换失败会自动跳过或创建报错。输入字符串具体是文件名还是路径仍需上游模型确认，不能把前四字节误当文件索引。S10/S14/S20 的意义、分配乘法溢出及失败处理仍待恢复。

## ed2k 包装

00478f4a 调 create_continued_emule_task2；0047917f 调 create_new_emule_task2。二者预处理和可见参数相同：S08、S0c、S10、S14、S18 都经 CW2AEX；**五个转换指针全部必须非 NULL**，否则返回 `0xffffffff`。可见调用只有五组指针/长度：

```text
create_{continued|new}_emule_task2(
  A(S08), L(S08), A(S0c), L(S0c), A(S10), L(S10),
  A(S14), L(S14), A(S18), L(S18))
```

S1c 被读入局部变量，但没有进入当前主程序伪代码的引擎调用。**后续直接反编译引擎的 10007518/10007717 已确认真实 task2 导出还有第 11 个输出任务句柄指针参数，并且入口先对它清零。上述十项仅是主程序伪码展示，绝非真实完整调用签名。** 共享序言、调用约定或 Ghidra 原型缺失确实影响了分析，尚未做主程序汇编级逐项栈验证；不可照抄十参数调用。

## 设置与返回值

`00476e2f` 将 Cookie 宽字符串转窄后调用 `set_cookie(S08, bytes, strlen)`；转换失败本地返回 -1。`00476ec6`、`00476f5d` 对文件名/完成文件名使用同样三项模式，但转换失败本地返回 **1**。因此不能把所有包装统一为“非零失败”或“零成功”。引擎返回值在这些成功路径均原样转发。

`0045643b` 调用 init(param_1)，仅在返回 **0** 时设置 `DAT_004c9c4c=1`。这是 init 的本地成功判据，不是创建任务返回值的通用定义。

### 上层返回码与恢复失败回退（补导后确认）

- `004793b4` 调普通新建包装 `0047863a`，返回值为 0 才返回 true。
- `0047940b` 调普通恢复 `004788ad`，非 0 将输出状态写为 `-0xc`。状态等于 -12 时转入本地文件检查/元数据重建路径（0044d8e3、0044db37），满足条件后再调用普通新建 0047863a，以返回 0 为成功。各 helper 细节还不能概括为“断点一定无损保留”。该路径将对象 `+0x74` 作为 PathCombineW 的目录、`+0x90` 作为第二路径分量；这两个局部语义有直接系统 API 证据，但不能因此恢复全对象结构。
- `0047970f` 调 ed2k 恢复 00478f4a，非 0 写 -12；遇到 -12 调 00456b78，再调用新建 0047917f。00456b78 的槽位 004c9c40 已确认是 **delete_emule_temp_files2**，它将四个宽字符串转为四组窄指针/长度后调用；原上层不检查这个清理包装的返回值。新建非 0 返回失败并保留 -12。这里存在清理临时文件再重建的分支，不能描述为纯粹无损重试；删除目标和引擎内部行为尚未恢复。
- `004798b5` 调 BT 恢复 00478c74，非 0 写 -12；遇到 -12 调 00468e3d，再调用新建 00478b09。成功后对象 +0x20 写 1。
- `00479984` 调 BT 新建 00478b09，将原返回值写对象 +0x20；非 0 写输出状态 `0xfffffff4`（-12），返回值等于 0 才返回 true。

因此可以说 **这些上层创建/恢复调用以返回 0 为成功，失败常规化为本地状态 -12**。后续引擎 1000640c/100064b0/1000a8be 又确认：**-12 同时也是引擎对无效/未注册任务句柄的直接返回值**。但不能因此认定上层所有 -12 都来自该底层原因，因为上层还会覆盖其他非零返回值。恢复失败后的重新创建是原版实际静态分支，新版应明确区分“继续现有数据”和“重新建立任务”，给用户解释错误与处理结果。引擎补证详见 `minithunder-engine-internals.md`。

已解析但尚未恢复可调用者语义的设置：set_speed_limit、cancel_speed_limit、set_res_use_strategy、set_thread_num、set_retry_strategy、set_upload_speed_limit、set_kad_enable、set_ed2k_enable、set_emule_kad_port。api-slot-references 中能见以下间接 CALL 指令地址：

| API | 指令地址 | 当前限制 |
|---|---|---|
| set_speed_limit | 004564a1 | 未归属到已完整反编译函数，参数未定 |
| set_kad_enable | 004564f8 | 同上 |
| set_ed2k_enable | 00456505 | 同上 |
| set_emule_kad_port | 00456516 | 同上 |
| set_thread_num | 00476ad5 | 同上 |
| set_retry_strategy | 本轮没有 READ 引用 | 仅证明被动态解析，不能宣称实际设置 |

`0047713a`、`00477286` 对 stat_para 的参数排序包含“长度在指针之前”的位置，与创建包装不同；应按真实槽位逐函数确认，不能套用统一字符串 ABI 模板。stat_para 的含义尚未知，不能将其叫作普通请求头或协议参数。

## 对新版的直接启示

- 创建、启动、停止是可分离接口：新版同样应将“持久化任务”“创建协议会话”“开始传输”分开，但原版最终状态流转还需上层调用者证明。
- 新版全程采用明确 Unicode 编码，不能复制旧版多处宽转窄再 strlen 的隐含约定。
- 普通、BT、ed2k 包装各有不同输入要求，适合统一上层任务模型、保持协议适配器输入各自强类型；不要把所有协议塞进一个无类型参数数组。
- 原表同时装载专有资源/统计/信用接口与标准协议接口；新版只复用行为知识，不加载该 DLL，也不复制迅雷网络相关 API。

## 下一轮最小补证

1. BT 输入宽字符串列表的上游来源，以及长路径/代码页不可表示字符时引擎如何处理。
2. 0042e157 的调用点：该 helper 已确认为 std::wstring 复制后 operator+=，但反编译遗漏调用参数，仍需汇编确定两库具体追加文件名。
3. 创建包装调用者：将 Sxx 对应到 URL、路径、元数据、输出任务标识，并确认创建返回码。
4. 包含 004564a1—00456516、00476ad5 的真实汇编范围：设置值与函数边界。
5. 对这六个包装做栈参数与 callee 清栈的汇编交叉检查，再考虑给函数命名/定义原型。当前不应向原 DLL 执行猜测 ABI 调用。
