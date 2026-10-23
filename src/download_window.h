#pragma once
#include <QMainWindow>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTimer>
#include <QRegularExpression>
#include <functional>
class DownloadWindow final : public QMainWindow {
public:
    std::function<void(QStringList)> dropped;
    DownloadWindow(){setAcceptDrops(true);}
protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if(event->mimeData()->hasUrls()||event->mimeData()->hasText())event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override {
        QStringList inputs;
        if(event->mimeData()->hasUrls()){
            const auto urls=event->mimeData()->urls();
            if(urls.size()>100)return;
            for(const auto& url:urls)inputs.append(url.isLocalFile()?url.toLocalFile():url.toString(QUrl::FullyEncoded));
        }else{
            const auto text=event->mimeData()->text();if(text.size()>65536)return;
            inputs=text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),Qt::SkipEmptyParts);
        }
        if(inputs.isEmpty()||inputs.size()>100)return;
        event->acceptProposedAction();
        QTimer::singleShot(0,this,[this,inputs]{if(dropped)dropped(inputs);});
    }
};

