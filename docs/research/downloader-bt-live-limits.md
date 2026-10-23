# BT 会话运行期限速

2026-10-21。TorrentEngine新增setRateLimits，向既有libtorrent session apply_settings，分别更新download_rate_limit/upload_rate_limit。全体BT任务共用一个session，因此是BT会话合计而不是每任务额度。rateLimits读取真实session get_settings供诊断与验证。负值在提交前拒绝，0不限速；沿用既有peer class配置，本机peer也纳入global class，不放行回环来掩盖速率测试。

TaskManager以torrentMutex串行处理懒加载会话与设置变更：会话未建时保存构造选项，已建时提交给同一会话。接口不改localOnly、peers或其他协议设置，也不新增Boost模块/库。Qt“BT限速”在同一对话框配置上传和下载KiB/s，取消不提交；初始上传仍为128KiB/s。明确仅本次运行、与普通下载分别计额、尚未覆盖ed2k。并非完整跨协议全局限速。

真实BT测试新增：从本机libtorrent seed下载既有4MiB双文件fixture，先64KiB/s下载/32KiB/s上传，读取真实session设置确认；开始有数据后保持至少1.5秒，验证增长小于512KiB（宽容块与调度突发），随后在同一活动会话改为双向不限速，要求完成且输出内容一致。还检查负速率拒绝后设置不变。该测试不精确证明64KiB/s线速，更不证明公网所有传输或实际上传吞吐：上传额度目前验证到真实session设置，主动做种与上传测速仍缺。

相关完整BT/BT对话框结果见logs/downloader-bt-live-limits-tests.log；构建日志logs/downloader-build-bt-live-limits.log。依赖守卫仍Boost44、无wxWidgets。

最终结果：BT集成测试通过（35.50秒），BT对话框测试通过（0.32秒），合计2/2通过。覆盖真实低速传输后修改限额完成，以及既有v1/v2/hybrid、磁力元数据、恢复和文件选择流程。本轮没有重复全部35项测试；上轮完整35/35结果仍按其原日志记录。

剩余：跨HTTP/BT/ed2k总额度分配、ed2k数据请求预算与上传、低占用开关、设置持久化、上传实际吞吐验收和原目标其他功能/逆向。不做迅雷加速网络。
