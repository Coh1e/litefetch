#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <algorithm>
#include <stdexcept>
namespace downloader {
// Shared payload budget. Blocking workers only; never wait on the GUI thread.
class BandwidthLimiter final {
public:
    void setRate(std::int64_t bytesPerSecond){
        if(bytesPerSecond<0||bytesPerSecond>1099511627776LL)throw std::invalid_argument("Invalid bandwidth limit");
        {std::lock_guard lock(mutex_);rate_=bytesPerSecond;credit_=0;updated_=Clock::now();}
        changed_.notify_all();
    }
    std::int64_t rate() const{std::lock_guard lock(mutex_);return rate_;}
    // Reserve an immediately spendable request size, never wait for a whole
    // large protocol block while holding a remote upload slot idle.
    std::int64_t acquire(std::int64_t maximum,const std::atomic_bool& cancelled){
        if(maximum<=0)throw std::invalid_argument("Invalid bandwidth reservation");
        std::unique_lock lock(mutex_);
        for(;;){
            if(cancelled.load())return 0;
            if(rate_==0)return maximum;
            const auto now=Clock::now();
            credit_=(std::min)((std::max)(1.0,rate_/10.0),credit_+std::chrono::duration<double>(now-updated_).count()*rate_);updated_=now;
            const auto grant=static_cast<std::int64_t>((std::min)(credit_,static_cast<double>(maximum)));
            if(grant>0){credit_-=grant;return grant;}
            changed_.wait_for(lock,std::chrono::milliseconds(25));
        }
    }
    bool consume(std::int64_t bytes,const std::atomic_bool& cancelled){
        if(bytes<0)throw std::invalid_argument("Invalid bandwidth charge");
        std::unique_lock lock(mutex_);double remaining=static_cast<double>(bytes);
        while(remaining>0){
            if(cancelled.load())return false;
            if(rate_==0)return true;
            const auto now=Clock::now();
            credit_=(std::min)((std::max)(1.0,rate_/10.0),credit_+std::chrono::duration<double>(now-updated_).count()*rate_);updated_=now;
            const auto taken=(std::min)(credit_,remaining);credit_-=taken;remaining-=taken;
            if(remaining<=0)return !cancelled.load();
            changed_.wait_for(lock,std::chrono::milliseconds(25));
        }
        return !cancelled.load();
    }
private:
    using Clock=std::chrono::steady_clock;
    mutable std::mutex mutex_;std::condition_variable changed_;
    std::int64_t rate_=0;double credit_=0;Clock::time_point updated_=Clock::now();
};
}
