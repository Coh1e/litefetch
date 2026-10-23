# 原生连接文件准备与Chrome联测前状态

日期：2026-10-20。尚未修改注册表或安装浏览器扩展。

## 已完成的本机准备

Qt设置对话框新增“准备连接文件”，与普通保存操作分开。明确触发后，先保存/应用本机偏好，再按当前配置导出可执行文件同目录的native-host.json和org.native_downloader.bridge.json。文件名、端点和扩展白名单来自同一份实际设置，避免与独立生成器的测试配置混用。

导出会检查host可执行文件存在、端点及来源格式。已有运行配置或host清单若不属于同一端点/身份/程序，会拒绝覆盖；相同配置可重复准备。每个文件用QSaveFile原子替换，两份文件不是一个跨文件事务；失败可能留下同一配置的部分准备文件，但不会把文件准备误报成浏览器已注册。

测试使用真实host可执行文件，验证显式按钮返回、文件内容一致、重复准备稳定、异配置拒绝覆盖和旧清单保留。原生窗口截图已检查，部署包启动通过。完整C++回归10/10通过，共40.35秒，日志logs/downloader-tests-with-browser-prepare.log；构建无编译警告。

## 临时注册授权及执行状态

2026-10-20 更新：用户已回复“允许临时注册，测试后恢复”。下文等待批准的描述为授权前记录；现在可按 Chrome 单项计划执行并恢复，不需要再次询问。实际注册仍未执行，先完成隔离浏览器启动验证。恢复逻辑及启动探测最新结果见 [downloader-browser-recovery-isolation.md](downloader-browser-recovery-isolation.md)。

本机已发现Chrome155.0.8059.84。标准安装路径未发现Edge；本轮拟先做Chrome联测，不提前修改Edge项。

只读检查表明当前用户Chrome/Edge的32/64位NativeMessagingHosts/org.native_downloader.bridge均不存在，记录保存到out/package/browser-connection/registration-before.json。这是检查时的状态，实际写入前必须再次核对。

Chrome单项计划：out/package/browser-connection/chrome-registration-plan.json。拟临时增加HKCU\Software\Google\Chrome\NativeMessagingHosts\org.native_downloader.bridge的默认字符串值，指向项目内org.native_downloader.bridge.json。联测后应恢复原先状态。尚未执行该动作，也未发出管理员权限请求。

用于隔离联测的host配置已准备到out/package/bin/native-host.json，指向项目内专用测试配置/端点和固定开发扩展身份；没有应用到个人浏览器资料。该测试配置与普通GUI默认任务目录分开，启动时须使用计划中的明确参数。

工作区README要求所有配置留在项目内。临时HKCU注册需要用户明确允许该例外；在得到答复前不执行注册。其他未完成项目仍可继续推进，不能因为该项等待批准而把整个目标宣布完成或阻塞。
