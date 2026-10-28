#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <exception>

namespace livekit::sdk::detail {
// One bounded, ordered control callback lane. Media/RPC workers stay independent.
class ControlExecutor {
 public:
  explicit ControlExecutor(size_t capacity,std::function<void()> failed):capacity_(capacity),failed_(std::move(failed)),thread_([this]{run();}){}
  ~ControlExecutor(){ {std::lock_guard lock(mutex_);stopping_=true;ready_.notify_all();} if(thread_.joinable())thread_.join(); }
  bool current()const{return thread_.get_id()==std::this_thread::get_id();}
  void post(std::function<void()> fn){
    std::lock_guard lock(mutex_);
    if(error_)std::rethrow_exception(error_);
    if(stopping_||queue_.size()>=capacity_)throw std::runtime_error("Control callback queue exhausted");
    queue_.push_back(std::move(fn));ready_.notify_one();
  }
  void drain(){std::unique_lock lock(mutex_);ready_.wait(lock,[&]{return queue_.empty()&&!active_;});if(error_)std::rethrow_exception(error_);}
 private:
  void run(){for(;;){
    std::function<void()> fn;
    {std::unique_lock lock(mutex_);ready_.wait(lock,[&]{return stopping_||!queue_.empty();});if(queue_.empty()&&stopping_)return;fn=std::move(queue_.front());queue_.pop_front();active_=true;}
    try{fn();}catch(...){ {std::lock_guard lock(mutex_);if(!error_)error_=std::current_exception();queue_.clear();}failed_(); }
    {std::lock_guard lock(mutex_);active_=false;ready_.notify_all();}
  }}
  size_t capacity_;std::function<void()> failed_;std::mutex mutex_;std::condition_variable ready_;
  std::deque<std::function<void()>> queue_;std::exception_ptr error_;bool active_=false,stopping_=false;std::thread thread_;
};
}
