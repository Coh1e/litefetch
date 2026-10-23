#include "download_window.h"
#include "desktop_tray.h"
#include "task_table.h"
#include <QApplication>
#include <QTemporaryDir>
#include <iostream>
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){QApplication app(argc,argv);try{
    DownloadWindow window;QStringList received;int calls=0;
    window.dropped=[&](QStringList inputs){received=inputs;++calls;};window.show();app.processEvents();
    const auto drop=[&](QMimeData& mime){
        QDragEnterEvent enter(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&window,&enter);
        QDropEvent event(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&window,&event);app.processEvents();return event.isAccepted();
    };
    QMimeData text;text.setText("https://example.invalid/a\r\n\nhttps://example.invalid/b");
    check(drop(text)&&calls==1&&received.size()==2,"Multiline drop was not delivered");
    QMimeData urls;const QString path="C:/fixture/with space.torrent";
    urls.setUrls({QUrl::fromLocalFile(path),QUrl("https://example.invalid/file")});
    check(drop(urls)&&calls==2&&received[0]==path&&received[1]=="https://example.invalid/file","File URL conversion failed");
    QMimeData oversized;oversized.setText(QString(65537,'x'));
    check(!drop(oversized)&&calls==2,"Oversized text was delivered");
    QMimeData many;QStringList lines;for(int i=0;i<101;++i)lines.append("https://example.invalid/"+QString::number(i));many.setText(lines.join('\n'));
    check(!drop(many)&&calls==2,"Excessive batch was delivered");
    QMimeData empty;empty.setText("\r\n");check(!drop(empty)&&calls==2,"Empty drop was delivered");
    downloader::DesktopTray tray(window);const bool available=tray.available();
    check(tray.hideWindow()==available,"Tray availability was not respected");
    check(window.isVisible()!=available,"Tray operation lost visible window without a tray");
    tray.restore();app.processEvents();check(window.isVisible(),"Tray restore did not show window");
    downloader::TaskTable table;table.setSortingEnabled(true);
    const auto update=[&](const QString& id,double percent,double speed){table.updateTask(id,[&](int row){
        table.item(row,0)->setText(id+".bin");table.item(row,0)->setData(Qt::UserRole+1,"https://example.invalid/"+id);
        table.item(row,1)->setText(id);table.item(row,2)->setText(QString::number(percent)+" %");
        table.item(row,2)->setData(downloader::taskSortRole,percent);
        table.item(row,3)->setText(QString::number(speed));table.item(row,3)->setData(downloader::taskSortRole,speed);
        table.item(row,4)->setText(id+"-state");
    });};
    update("a",2,900);update("b",100,10);update("c",10,1000);
    table.sortItems(2,Qt::AscendingOrder);
    check(table.item(0,1)->text()=="a"&&table.item(1,1)->text()=="c"&&table.item(2,1)->text()=="b","Progress sorted by display text");
    table.setCurrentCell(1,0);check(table.selectedTaskId()=="c","Selection fixture failed");
    for(int i=0;i<50;++i){update("c",i%2?1:101,1000);
        check(table.selectedTaskId()=="c","Sorted updates switched selected identity");
        for(int row=0;row<table.rowCount();++row){const auto id=table.item(row,0)->data(Qt::UserRole).toString();
            check(table.item(row,1)->text()==id&&table.item(row,4)->text()==id+"-state","Sorted update mixed different task cells");}
    }
    table.sortItems(3,Qt::AscendingOrder);check(table.item(0,1)->text()=="b"&&table.item(2,1)->text()=="c","Speed sort is not numeric");
    table.setTaskFilter("EXAMPLE.INVALID/a");int visible=0;for(int row=0;row<table.rowCount();++row)if(!table.isRowHidden(row)){++visible;check(table.item(row,1)->text()=="a","Source filter hid wrong identity");}
    check(visible==1&&table.selectedTaskId().isEmpty(),"Hidden selection could still target a task");
    table.sortItems(2,Qt::DescendingOrder);update("b",50,10);visible=0;
    for(int row=0;row<table.rowCount();++row)if(!table.isRowHidden(row)){++visible;check(table.item(row,1)->text()=="a","Sorting moved filter to another task");}
    check(visible==1,"Sorting lost active filter");
    table.setTaskFilter({});table.setCurrentCell(0,0);const auto removed=table.selectedTaskId();table.updateTask(removed,{});
    check(table.rowCount()==2&&table.selectedTaskId().isEmpty(),"Removing sorted selection targeted a neighbor");
    std::cout<<"Native drop delivery and bounds passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
