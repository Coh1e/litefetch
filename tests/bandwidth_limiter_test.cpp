#include "bandwidth_limiter.h"
#include <future>
#include <thread>
#include <iostream>
using namespace std::chrono_literals;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    downloader::BandwidthLimiter limit;std::atomic_bool cancelled=false;
    check(limit.consume(1000000,cancelled),"Unlimited budget failed");
    limit.setRate(1);auto blocked=std::async(std::launch::async,[&]{return limit.consume(65536,cancelled);});
    check(blocked.wait_for(50ms)==std::future_status::timeout,"Limited operation did not wait");
    cancelled=true;check(blocked.wait_for(500ms)==std::future_status::ready&&!blocked.get(),"Cancellation did not interrupt rate wait");
    cancelled=false;limit.setRate(1);auto changed=std::async(std::launch::async,[&]{return limit.consume(65536,cancelled);});
    check(changed.wait_for(50ms)==std::future_status::timeout,"Second limited operation did not wait");
    limit.setRate(0);check(changed.wait_for(500ms)==std::future_status::ready&&changed.get(),"Disabling limit did not wake worker");
    limit.setRate(1);auto reservation=std::async(std::launch::async,[&]{return limit.acquire(65536,cancelled);});
    check(reservation.wait_for(50ms)==std::future_status::timeout,"Reservation waited for no credit incorrectly");
    cancelled=true;check(reservation.wait_for(500ms)==std::future_status::ready&&reservation.get()==0,"Reservation cancellation failed");
    cancelled=false;limit.setRate(0);check(limit.acquire(65536,cancelled)==65536,"Unlimited reservation was truncated");
    bool rejected=false;try{limit.setRate(-1);}catch(const std::invalid_argument&){rejected=true;}check(rejected&&limit.rate()==0,"Invalid rate changed configuration");
    std::cout<<"Rate waits, cancellation and live update passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
