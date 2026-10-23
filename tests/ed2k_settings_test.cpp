#include "ed2k_settings.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QFile>
#include <QTimer>
#include <iostream>
using namespace downloader;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Expected rejection");}
int main(int argc,char** argv){QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);try{
    QTemporaryDir dir(app.applicationDirPath()+"/ed2k-settings-XXXXXX");check(dir.isValid(),"Test directory failed");
    const auto path=dir.filePath("ed2k.json");auto defaults=loadEd2kPreferences(path);
    check(!defaults.enabled&&defaults.options.userHash.size()==16&&!QFile::exists(path),"Defaults touched configuration");
    bool interacted=false,controlsValid=false;
    QTimer::singleShot(100,&app,[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog){app.exit(2);return;}
        auto* enabled=dialog->findChild<QCheckBox*>("ed2kEnabled");auto* address=dialog->findChild<QLineEdit*>("ed2kServerAddress");
        auto* port=dialog->findChild<QSpinBox*>("ed2kServerPort");auto* nickname=dialog->findChild<QLineEdit*>("ed2kNickname");auto* buttons=dialog->findChild<QDialogButtonBox*>("ed2kButtons");
        if(!enabled||!address||!port||!nickname||!buttons){dialog->reject();return;}
        enabled->setChecked(true);address->setText("not-an-ip");const bool rejectsAddress=!buttons->button(QDialogButtonBox::Save)->isEnabled();
        address->setText("127.0.0.1");port->setValue(4661);nickname->setText(QString::fromUtf8("测试"));const bool rejectsEncoding=!buttons->button(QDialogButtonBox::Save)->isEnabled();
        nickname->setText("NativeDownloader");controlsValid=rejectsAddress&&rejectsEncoding&&buttons->button(QDialogButtonBox::Save)->isEnabled();interacted=true;
        if(app.arguments().contains("--image"))dialog->grab().save(app.arguments().value(app.arguments().indexOf("--image")+1));
        buttons->button(QDialogButtonBox::Save)->click();
    });
    const auto selected=chooseEd2kPreferences(nullptr,defaults);check(interacted&&controlsValid&&selected&&selected->enabled,"Native ed2k dialog failed");
    check(!QFile::exists(path),"Dialog implicitly saved settings");saveEd2kPreferences(path,*selected);const auto restored=loadEd2kPreferences(path);
    check(restored.enabled&&restored.options.server==QHostAddress::LocalHost&&restored.options.userHash==defaults.options.userHash,"Settings or stable identity changed");
    auto disabled=restored;disabled.enabled=false;saveEd2kPreferences(path,disabled);
    check(!validEd2kOptions(ed2kRuntimeOptions(loadEd2kPreferences(path)))&&loadEd2kPreferences(path).options.server==QHostAddress::LocalHost,"Disabling lost endpoint or stayed enabled");
    QTimer::singleShot(100,&app,[&]{if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();else app.exit(2);});
    check(!chooseEd2kPreferences(nullptr,restored),"Cancelled settings accepted");
    bool kadControls=false;
    QTimer::singleShot(100,&app,[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog){app.exit(2);return;}
        auto* mode=dialog->findChild<QComboBox*>("ed2kSourceMode");auto* seed=dialog->findChild<QLineEdit*>("kadSeedAddress");
        auto* port=dialog->findChild<QSpinBox*>("kadSeedPort");auto* buttons=dialog->findChild<QDialogButtonBox*>("ed2kButtons");
        if(!mode||!seed||!port||!buttons){dialog->reject();return;}
        mode->setCurrentIndex(1);const bool emptyRejected=!buttons->button(QDialogButtonBox::Save)->isEnabled();seed->setText("::1");
        const bool ipv6Rejected=!buttons->button(QDialogButtonBox::Save)->isEnabled();seed->setText("127.0.0.1");port->setValue(4672);
        kadControls=emptyRejected&&ipv6Rejected&&buttons->button(QDialogButtonBox::Save)->isEnabled();
        if(app.arguments().contains("--kad-image")){const auto image=app.arguments().value(app.arguments().indexOf("--kad-image")+1);
            QTimer::singleShot(180,dialog,[dialog,buttons,image]{dialog->grab().save(image);buttons->button(QDialogButtonBox::Save)->click();});
        }else buttons->button(QDialogButtonBox::Save)->click();
    });
    const auto kad=chooseEd2kPreferences(nullptr,restored);check(kadControls&&kad&&kad->options.useKad&&kad->options.kadSeeds.size()==1,"Kad Qt controls failed");
    saveEd2kPreferences(path,*kad);const auto loadedKad=loadEd2kPreferences(path);
    check(loadedKad.options.useKad&&loadedKad.options.kadSeeds.first().first==QHostAddress::LocalHost&&loadedKad.options.userHash==defaults.options.userHash,"Kad settings not persisted");
    auto kadOff=loadedKad;kadOff.enabled=false;saveEd2kPreferences(path,kadOff);
    check(!validEd2kOptions(ed2kRuntimeOptions(loadEd2kPreferences(path)))&&loadEd2kPreferences(path).options.kadSeeds.size()==1,"Disabled Kad still active or lost seed");
    {
        QFile current(path);check(current.open(QIODevice::ReadOnly),"Config read failed");auto legacy=QJsonDocument::fromJson(current.readAll()).object();current.close();
        legacy["version"]=1;legacy.remove("mode");legacy.remove("kadSeeds");legacy.remove("kadTimeoutMs");
        QFile old(dir.filePath("legacy.json"));check(old.open(QIODevice::WriteOnly)&&old.write(QJsonDocument(legacy).toJson())>0,"Legacy config fixture failed");old.close();
        check(!loadEd2kPreferences(old.fileName()).options.useKad&&loadEd2kPreferences(old.fileName()).options.userHash==defaults.options.userHash,"Version-1 config migration changed identity");
    }
    QFile bad(path);check(bad.open(QIODevice::WriteOnly)&&bad.write("{broken")==7,"Invalid config fixture failed");bad.close();
    rejects([&]{loadEd2kPreferences(path);});
    std::cout<<"ed2k Qt settings and persistence tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
