# ed2k 原生控制客户端

2026-10-20。src/amule_api.h/.cpp 新增阻塞式 C++ aMule API 客户端，作为后续后台生命周期与统一任务适配的基础。仅连接 127.0.0.1 和明确端口；认证后使用 Bearer token。无全局配置修改，不跟随重定向，不使用环境代理，连接/请求有界，响应最多16 MiB，错误不带密码/token/响应正文。变更操作不自动重试，避免超时后不明状态的重复添加。

支持 health、添加已校验 ed2k 链接、按32位十六进制 hash查询、暂停、继续和优先级。快照保留引擎原始状态；核对 hash、非负计数和 completed<=size，不把未知状态擅自映射为完成。它尚不管理 aMule 进程，也未接入 TaskManager。

## 真实后台验证

使用已固定的 aMule 3.1.0 Windows 便携后台，关闭公网自动连接、ed2k/Kad、UPnP与更新，所有配置/数据/临时文件在项目内。scripts/probe-amule.ps1 新增可选 -ClientTest，运行实际 download-amule-api-tests.exe；测试密码通过项目内临时文件传入，finally 删除。测试结束正常关闭后台。

客户端实测错误密码401、正确认证、EC连接、添加32 KiB无来源任务、查询、暂停、继续、优先级回读，以及非法hash和非法进度快照拒绝。新建辅助任务成功后由测试脚本删除，原有合成任务仍保留给恢复阶段。脚本再执行 restore：原有13项离线检查（create6、restore7）均通过，确认原有暂停意图/优先级/限速恢复流程未被破坏。

证据：logs/downloader-build-ed2k-api.log、logs/amule-native-api-live.log、logs/downloader-spike/native-client.stdout.log、logs/downloader-spike/native-client.stderr.log、logs/amule-native-api-restore.log。CMake目标 download-ed2k 与 download-amule-api-tests；因测试要求实际后台生命周期，未把它作为没有fixture的普通CTest假通过。运行方式：scripts/probe-amule.ps1 -Mode run -Stage create -ClientTest downloader/out/build/windows-local/download-amule-api-tests.exe，再执行 -Stage restore。

## 未完成

仍需随机凭据/端口与受控进程生命周期、TaskManager和GUI接入、按任务保存用户暂停意图、实际 ed2k 文件传输和部分数据恢复、完成文件发布/校验/同名冲突、Kad/服务器及代理验证、aMule源码构建与分发许可核对。当前 GUI 仍明确拒绝尚未接入的 ed2k 任务；不将控制API成功当成实际下载可用。

接口依据为本地上游源码 work/downloader-spike/source/amule-org-amule-6ccbfdf/src/webapi/Api.cpp 和此前便携后台实测，不依赖猜测的私有迅雷接口。不加载或分发原迅雷引擎。
