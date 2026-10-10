#pragma once
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
namespace kon {
// A synchronous barrier per draw preserves draw/blend order. Workers only
// shade disjoint output rows; guest execution never overlaps their lifetime.
class NativeRasterPool {
public:
    explicit NativeRasterPool(unsigned lanes=4):lanes_(std::clamp(lanes,1u,16u)){}
    NativeRasterPool(const NativeRasterPool&)=delete;
    ~NativeRasterPool(){
        {std::lock_guard lock(mutex_);stopping_=true;}
        changed_.notify_all();for(auto& worker:workers_)worker.join();
    }
    unsigned lanes()const{return lanes_;}
    void run(const std::function<void(unsigned)>& task){
        if(lanes_==1){task(0);return;}
        if(workers_.empty())for(unsigned id=1;id<lanes_;++id)workers_.emplace_back([this,id]{work(id);});
        {std::lock_guard lock(mutex_);task_=&task;completed_=0;error_=nullptr;++generation_;}
        changed_.notify_all();
        try{task(0);}catch(...){std::lock_guard lock(mutex_);error_=std::current_exception();}
        std::unique_lock lock(mutex_);finished_.wait(lock,[this]{return completed_==lanes_-1;});
        task_=nullptr;const auto error=error_;lock.unlock();if(error)std::rethrow_exception(error);
    }
private:
    unsigned lanes_,completed_=0;
    std::uint64_t generation_=0;
    bool stopping_=false;
    std::mutex mutex_;std::condition_variable changed_,finished_;
    std::vector<std::thread> workers_;
    const std::function<void(unsigned)>* task_=nullptr;
    std::exception_ptr error_;
    void work(unsigned id){
        std::uint64_t seen=0;
        for(;;){
            std::unique_lock lock(mutex_);changed_.wait(lock,[&]{return stopping_ || generation_!=seen;});
            if(stopping_)return;
            seen=generation_;auto task=task_;lock.unlock();
            std::exception_ptr error;try{(*task)(id);}catch(...){error=std::current_exception();}
            lock.lock();if(error && !error_)error_=error;++completed_;lock.unlock();finished_.notify_one();
        }
    }
};
}
