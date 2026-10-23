#pragma once
#include "download_request.h"
#include <QSet>
namespace downloader {
using BatchInputs=std::variant<QStringList,InvalidRequest>;
// No tasks or files are created until all input rows pass format validation.
inline BatchInputs prepareBatch(const QStringList& lines){
    if(lines.size()>100)return InvalidRequest{QStringLiteral("每次最多添加100项。")};
    QStringList inputs;QSet<QString> seen;qsizetype characters=0;
    for(qsizetype i=0;i<lines.size();++i){
        characters+=lines[i].size();
        if(characters>65536)return InvalidRequest{QStringLiteral("输入内容过长，请分批添加。")};
        const auto text=lines[i].trimmed();if(text.isEmpty())continue;
        const auto parsed=parseRequest(text);
        if(const auto* error=std::get_if<InvalidRequest>(&parsed))
            return InvalidRequest{QStringLiteral("第%1项：%2").arg(i+1).arg(error->message)};
        const auto& source=std::get<DownloadRequest>(parsed).source;
        if(!seen.contains(source)){seen.insert(source);inputs.append(text);}
    }
    return inputs;
}
}
