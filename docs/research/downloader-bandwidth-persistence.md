# 限速设置持久化

2026-10-21。项目数据目录新增bandwidth.json，schema1，保存directDownload（HTTP/FTP/ed2k共享下载）、btDownload和btUpload，单位字节/秒。每项0..1073741824，0不限速；初始仍为普通不限速、BT下载不限速/上传128KiB/s。配置最多4096字节，必须是对象、已知版本，三项均为整数；不接受字符串、缺项、负数、分数、超限或损坏JSON。

加载发生在TaskManager创建前；BT构造选项从配置初始化，普通共享预算在构造完成后、首次事件循环/排队调度前设置。读取失败不删除或重写原文件，使用默认值并在状态栏提示。用户显式确认设置时先以QSaveFile原子写入（禁用直接覆盖回退），写入成功后更新运行限额；保存失败不执行运行限额更新。菜单与对话框已移除“仅本次有效”，明确自动保存，界面保持Everything方向的原生菜单和紧凑列表。

独立测试download-bandwidth-settings覆盖缺失默认、保存重载、非法保存保留旧文件、损坏/缺项/类型/版本/分数/上界、超过4KiB和写入失败；与既有限速器测试2/2通过，logs/downloader-bandwidth-settings-tests.log。构建通过且无新增库。上轮完整35项回归不改称本轮结果，本轮未再次跑所有网络协议。

真实应用独立进程：scripts/probe-bandwidth-startup.ps1创建项目内隔离数据目录；有效配置恢复256KiB/s共享下载、512KiB/s BT下载、64KiB/s BT上传，独立损坏配置启动回退0/0/128KiB/s，两次退出均0。分别比对启动前后配置SHA256，均未改写。证据work/bandwidth-startup/3461e0cfb3fb4cc98539cdb100c333b2，logs/downloader-bandwidth-startup.log。报告读取的是TaskManager实际运行配置；没有BT任务时不创建公网BT会话。并非模拟点击设置对话框后重启的全套GUI验收。

尚未完成跨BT/HTTP/ed2k总额度分配、低占用开关、ed2k上传、计划时段限速等原范围。不得将三项分别持久化写成跨核心全局限速已完成；不做迅雷加速网络。
