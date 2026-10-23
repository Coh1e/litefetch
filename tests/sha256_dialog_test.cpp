#include "sha256_dialog.h"
#include <QApplication>
#include <QDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QFile>
#include <QTimer>
#include <QElapsedTimer>
#include <iostream>
int main(int argc,char** argv){QApplication app(argc,argv);QTemporaryDir directory;
    const auto path=directory.filePath("abc.bin");QFile file(path);
    if(!file.open(QIODevice::WriteOnly)||file.write("abc",3)!=3)return 2;file.close();
    int stage=0;bool matched=false,timedOut=false;QElapsedTimer elapsed;elapsed.start();QTimer timer;
    QObject::connect(&timer,&QTimer::timeout,&app,[&]{
        auto* modal=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(elapsed.elapsed()>5000){timedOut=true;if(modal)modal->reject();return;}
        if(auto* input=qobject_cast<QInputDialog*>(modal);input&&stage==0){
            input->setTextValue("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");stage=1;input->accept();
        }else if(auto* message=qobject_cast<QMessageBox*>(modal);message&&stage==1){
            matched=message->icon()==QMessageBox::Information&&message->text()==QStringLiteral("校验一致。");stage=2;message->accept();
        }
    });timer.start(10);
    downloader::showSha256Verification(nullptr,path);timer.stop();
    if(timedOut||stage!=2||!matched){std::cerr<<"Native hash dialog flow failed\n";return 1;}
    std::cout<<"Native expected hash input, background verification and matching result passed\n";return 0;
}
