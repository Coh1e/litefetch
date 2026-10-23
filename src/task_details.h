#pragma once
#include "task_manager.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QUrl>

namespace downloader {
inline QPair<QString,QString> taskErrorExplanation(TransferError error)
{
    switch(error){
    case TransferError::InvalidRequest:return {QStringLiteral("请求或配置无效"),QStringLiteral("检查下载地址及对应协议设置后重试。")};
    case TransferError::Destination:return {QStringLiteral("保存位置冲突或不可用"),QStringLiteral("检查目标路径、同名文件及目录访问权限；已有文件不会被覆盖。")};
    case TransferError::Network:return {QStringLiteral("网络连接或传输失败"),QStringLiteral("检查网络及来源可用性后尝试继续。")};
    case TransferError::Http:return {QStringLiteral("服务器响应异常"),QStringLiteral("检查下方服务器原因及链接是否仍有效；部分来源可能需要重新获取链接。")};
    case TransferError::Tls:return {QStringLiteral("安全连接失败"),QStringLiteral("检查系统时间、证书信任和来源地址；不要通过关闭证书验证解决。")};
    case TransferError::Disk:return {QStringLiteral("磁盘或恢复数据读写失败"),QStringLiteral("检查磁盘空间、目录权限及存储设备后尝试继续。")};
    case TransferError::ResourceChanged:return {QStringLiteral("资源或续传数据不一致"),QStringLiteral("先核对原链接和来源。确认需要重新下载后选择从头重试，旧暂存数据可能被清理。")};
    case TransferError::Internal:return {QStringLiteral("程序内部处理失败"),QStringLiteral("保留最近原因及任务信息，重启后重试。")};
    default:return {QStringLiteral("未分类"),QStringLiteral("历史记录或该路径尚无具体分类，请参考下方最近原因。")};
    }
}
// A value snapshot: modal event processing may change the manager's task vector.
inline void showTaskDetails(QWidget* parent,Task task)
{
    QDialog dialog(parent);dialog.setWindowTitle(QStringLiteral("任务详情"));dialog.resize(640,420);
    auto* layout=new QVBoxLayout(&dialog);
    auto* text=new QPlainTextEdit(&dialog);text->setReadOnly(true);
    const auto protocol=task.request.protocol==Protocol::BitTorrent?QStringLiteral("BT / 磁力链"):
        task.request.protocol==Protocol::Ed2k?QStringLiteral("ed2k"):
        task.request.protocol==Protocol::Ftp?QStringLiteral("FTP"):QStringLiteral("HTTP(S)");
    auto source=task.request.source;
    if(task.request.protocol==Protocol::Http||task.request.protocol==Protocol::Ftp)
        source=QUrl(source).toString(QUrl::RemovePassword);
    QStringList lines{
        QStringLiteral("任务：%1").arg(task.id),
        QStringLiteral("协议：%1").arg(protocol),
        QStringLiteral("状态：%1").arg(taskStateText(task.state)),
        QStringLiteral("来源：%1").arg(source),
        QStringLiteral("保存到：%1").arg(task.destination),
        QStringLiteral("已接收：%1 字节").arg(task.received),
        task.total>0?QStringLiteral("总大小：%1 字节").arg(task.total):QStringLiteral("总大小：尚未确定"),
        QStringLiteral("优先级：%1").arg(task.priority),QString{},
        QStringLiteral("最近原因："),task.error.isEmpty()?QStringLiteral("无"):task.error};
    if(task.state==TaskState::Failed){
        const auto explanation=taskErrorExplanation(task.errorType);
        lines<<QString{}<<QStringLiteral("错误分类：%1").arg(explanation.first)<<QStringLiteral("建议：%1").arg(explanation.second);
    }
    if(task.request.protocol==Protocol::BitTorrent){
        lines<<QString{}<<(task.uploaded>=0?QStringLiteral("累计上传（核心最近上报）：%1 字节").arg(task.uploaded):QStringLiteral("累计上传：尚未由核心上报"));
        lines<<QStringLiteral("上传速度（最近上报）：%1 KiB/s").arg(task.uploadBytesPerSecond/1024.0,0,'f',1);
        if(task.state==TaskState::Running&&task.phase==TransferPhase::Seeding)lines<<QStringLiteral("当前阶段：做种中");
    }
    if(task.state==TaskState::Failed||task.state==TaskState::Paused||task.state==TaskState::Interrupted){
        lines<<QString{}<<QStringLiteral("可关闭此窗口后尝试“继续”。“从头重试”可能清理暂存数据；BT 的“重试并校验”会重新检查已有数据。");
    }
    lines<<QString{}<<QStringLiteral("以上是打开窗口时的任务快照；后台下载继续运行。");
    text->setPlainText(lines.join('\n'));layout->addWidget(text);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,&dialog);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    layout->addWidget(buttons);dialog.exec();
}
}
