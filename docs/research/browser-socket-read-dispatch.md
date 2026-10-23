# 浏览器接管 socket 的异步接收端与栈参数

后续见[browser-socket-manager-factory.md](browser-socket-manager-factory.md)：管理器查询键、样本内+74工厂和返回适配接口已补静态证据，核查扩展到20项；服务选择与完整底层桥接/动态行为仍未闭合。

2026-10-21。承接browser-prebuffer-read.md。本轮只读取固定download_engine.dll与既有Ghidra工程，没有运行样本、注册组件或发出样本网络请求。SHA256仍38dd7f676a592bf1810e314e370b28fa4a43e2df3e6a8181447335333d1b0083。

## 新证据与反编译限制

100bc170初始化设备虚表1037de18，将第一实参（上层先经100bc420构造的socket包装器）保存到对象+4，置对象+8为0。原断言字符串为speed_limit_ptr != NULL && _socket_imp_ptr != NULL，源文件limit_asyn_socket_device.cpp。这里应区分包装器、限速参数对象及最终异步接口，不能把对象+8未经核对就称作原始socket。

关键原指令：100bc1ee/1ef/1f5先压入输出地址（设备+8）、0和第二实参+4所取值；ECX=第一实参，100bc1f8调用其虚表+24。1037de64+24指向10167870，后者只有MOV EAX,[ECX+4]; RET（无栈清理）。因此先压入的三个值没有被getter消费。随后100bc1fb压入getter返回的底层指针、100bc200压入管理器，100bc201调用管理器虚表+74。按实际栈顺序，这三个值是为后一个管理器调用准备的。反编译把它们显示成getter的参数不可信；不据此强改数据库原型。

管理器+74调用的静态实参顺序可记录为（管理器、底层socket对象、第二实参+4所取值、0、设备+8输出地址），但+74目标及最终返回接口类型仍未完整恢复。不能把这项栈参数纠正写成全部接口闭环。

## 底层接收块

已有socket工厂10034080在WSASocketW后调用1004b7d0。构造指令1004b808把对象虚表设为10377484，1004b80e把句柄保存到对象+10；该表+4槽为1004af90。此入口的原指令块在1004b0a9经IAT1036f778调用WSARecv。没有人为建立或扩大1004af90的函数边界，证据以虚表字节和已有指令为准。

调用分支在1004b075和1004b08e分别准备空completion routine或10038340，使用同一OVERLAPPED地址；返回-1后取错误并比较0x3e5，进入等待完成相关分支，而不是一概作同步失败。导入引用另定位到1003f6b0的WSAGetOverlappedResult，以及10038420的GetQueuedCompletionStatus循环、PostMessageA(...,0x401,...)投递。它们证明引擎具有异步接收/完成处理路径，不单凭名称把它们全部连成浏览器接管对象的一条已验证动态链。

## 产物与复核

scripts/ExportSocketRead.java与export-socket-read.ps1以-readOnly/-noanalysis导出现有项目，输出analysis/exports/minithunder-socket-read/socket-read-instructions.txt与socket-read-functions.jsonl。最终导出51个已有函数，51个反编译完成；有完整伪代码不等于原型正确，仍以指令核对。没有生成推测函数。

scripts/verify-socket-read.mjs对原PE做8项静态核查：包装器getter槽、getter不清栈的字节、getter/管理器调用指令、raw接收槽、构造写虚表、WSARecv IAT调用及pending比较。全部通过，socket-read-byte-checks.json标明runtimeVerified=false。样本哈希前后相同。

首次默认受限环境打开工程报“Project ... is read-only”，日志socket-read-focus.log保留；同一只读分析在允许Java访问项目工作数据库/临时锁的环境下成功，日志socket-read-focus-complete.log。日志明确Processing read-only project file，没有Save succeeded。未对产品代码或依赖作修改。

仍缺：管理器+74到最终适配对象的目标与转发、完整接收回调到普通/chunked管道、下游失败传播与落盘、原版浏览器实际取消/失败回退、动态文件一致性及HTTPS等边界。现代浏览器仍用公开扩展API和已实现的持久交接，不复制原版socket层，不复用闭源DLL，不做迅雷加速网络。
