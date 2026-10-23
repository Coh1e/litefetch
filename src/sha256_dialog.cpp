#include "sha256_dialog.h"
#include "sha256_verify.h"
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTimer>
#include <QCloseEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QFileInfo>
#include <atomic>
#include <future>
namespace downloader {
namespace {
class VerifyDialog final:public QDialog {
public:
    Sha256Result result;
    VerifyDialog(QWidget* parent,const QString& path,const QString& expected):QDialog(parent){
        setWindowTitle(QStringLiteral("SHA-256 校验"));setMinimumWidth(400);
        auto* layout=new QVBoxLayout(this);label_=new QLabel(QStringLiteral("正在读取文件…"),this);
        auto* bar=new QProgressBar(this);bar->setRange(0,1000);button_=new QPushButton(QStringLiteral("取消"),this);
        layout->addWidget(label_);layout->addWidget(bar);layout->addWidget(button_);
        connect(button_,&QPushButton::clicked,this,&VerifyDialog::reject);
        future_=std::async(std::launch::async,[this,path,expected]{return verifySha256(path,expected,[this]{return cancelled_.load();},
            [this](qint64 read,qint64 total){received_=read;total_=total;});});
        timer_.setInterval(50);connect(&timer_,&QTimer::timeout,this,[this,bar]{
            const auto total=total_.load();if(total>0)bar->setValue(int(1000.0*received_.load()/total));
            if(future_.wait_for(std::chrono::seconds(0))==std::future_status::ready){result=future_.get();done_=true;timer_.stop();accept();}
        });timer_.start();
    }
    void reject() override{cancelled_=true;button_->setEnabled(false);label_->setText(QStringLiteral("正在取消…"));}
protected:
    void closeEvent(QCloseEvent* event) override{if(done_)QDialog::closeEvent(event);else{reject();event->ignore();}}
private:
    std::atomic_bool cancelled_=false;std::atomic<qint64> received_=0,total_=0;
    std::future<Sha256Result> future_;QTimer timer_;QLabel* label_;QPushButton* button_;bool done_=false;
};
}
void showSha256Verification(QWidget* parent,QString path){
    bool accepted=false;const auto expected=QInputDialog::getText(parent,QStringLiteral("SHA-256 校验"),
        QStringLiteral("文件：%1\n输入来源提供的SHA-256（64位十六进制）").arg(QFileInfo(path).fileName()),QLineEdit::Normal,{},&accepted).trimmed();
    if(!accepted)return;
    if(!validSha256(expected)){QMessageBox::information(parent,QStringLiteral("检查校验值"),QStringLiteral("请输入64位十六进制SHA-256。"));return;}
    VerifyDialog dialog(parent,path,expected);dialog.exec();const auto& result=dialog.result;
    if(result.status==Sha256Status::Cancelled)return;
    if(result.status==Sha256Status::Matched)QMessageBox::information(parent,QStringLiteral("SHA-256 校验"),QStringLiteral("校验一致。"));
    else if(result.status==Sha256Status::Mismatch)QMessageBox::warning(parent,QStringLiteral("校验不一致"),QStringLiteral("文件SHA-256与输入值不同。\n实际值：%1").arg(result.actual));
    else QMessageBox::warning(parent,QStringLiteral("校验未完成"),result.message);
}
}
