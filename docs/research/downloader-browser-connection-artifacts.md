# 固定开发扩展身份与连接清单

日期：2026-10-20。仅生成项目内文件；未安装扩展、未写注册表、未修改个人浏览器配置。

## 身份

manifest.json现含固定RSA公钥key，开发扩展ID为`namdpafnjehflfkcgdbnmoccndohmjoj`，允许来源为`chrome-extension://namdpafnjehflfkcgdbnmoccndohmjoj/`。公钥不是凭据。本轮生成密钥仅用于未打包开发扩展身份，没有持久化私钥，也没有制作签名CRX；未来商店发布ID需另行确认，不能把开发ID当作已取得的商店身份。

scripts/ensure-browser-identity.mjs仅在缺少key时初始化；已有key不会自动轮换。scripts/browser-identity.mjs按公钥DER的SHA-256前16字节映射a-p生成ID，并校验公钥格式。依据：[Chrome manifest key](https://developer.chrome.com/docs/extensions/reference/manifest/key)、[Chromium id_util.cc](https://chromium.googlesource.com/chromium/src/+/main/components/crx_file/id_util.cc)。真实浏览器显示的ID仍待核对。

## 已生成的可审阅文件

scripts/prepare-browser-connection.mjs在downloader/out/package/browser-connection生成：

1. org.native_downloader.bridge.json：Native Messaging host清单，指向当前package/bin/native-downloader-host.exe，只允许上述开发扩展来源。
2. native-host.json：host运行配置预览，包含同一来源与本机端点；尚未复制到host可执行文件旁。
3. browser-bridge.json：应用服务配置预览，指向项目内专用测试下载目录。需通过显式开发启动选项使用，未自动启动应用。
4. registration-plan.json：列出应用启动参数、待复制文件，以及Chrome/Edge各一个HKCU默认字符串值的拟注册位置。reviewOnly=true、registryChanged=false。

计划使用当前用户Registry64；实际执行前仍须检查32位和64位既有注册，备份原值并处理冲突。当前没有执行注册/覆盖/卸载，不把计划当安装结果。官方依据：[Chrome Native Messaging注册](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging#native-messaging-host-location)、[Edge Native Messaging](https://learn.microsoft.com/en-us/microsoft-edge/extensions/developer-guide/native-messaging)。

工作区README要求“所有安装、依赖、配置、缓存、临时文件、日志及分析产物必须位于本目录内”。HKCU注册是项目外配置，实际执行需用户明确允许例外；生成项目内计划本身不需要修改该约束。原生设置界面尚待实现，不提前要求用户执行安装步骤。

## 验证与修正

- 配置生成器限制输出、数据目录、下载目录和host二进制都在选定项目内；写入前检查最近已有父目录的真实路径，拒绝外部目录及越界联接。
- Node测试包含固定ID、重复生成不变、四份文件的一致性、未复制运行配置/未执行注册，以及外部路径与目录联接拒绝。
- 61项浏览器/配置测试通过，日志logs/browser-connection-tests.log。联接测试首次因沙箱不允许创建reparse point而失败；在项目内隔离目录获准复验后通过，未跳过该用例。
- host接收的调用来源可带或不带末尾斜杠，验证32位a-p扩展ID后统一为清单中的带斜杠origin；不放宽主机名或接受任意路径。真实host来源归一化测试及host→IPC→HTTP整链均通过，见downloader-tests-native-host-identity.log和downloader-tests-browser-bridge-identity.log。

Native Messaging注册、真实Chrome/Edge加载、用户设置体验、Firefox与登录上下文仍未验收。Qt GUI与host已重新部署，9文件开发扩展包已带固定key重新打包。
