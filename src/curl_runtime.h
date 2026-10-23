#pragma once
#include <curl/curl.h>
namespace downloader::detail {
inline CURLcode initializeCurl()
{
    struct Runtime {
        CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        ~Runtime() { if (result == CURLE_OK) curl_global_cleanup(); }
    };
    static Runtime runtime;
    return runtime.result;
}
}
