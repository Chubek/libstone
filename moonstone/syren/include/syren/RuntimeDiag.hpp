#pragma once
#include "../../../core/Core.hpp"
#include <mutex>
#include <deque>
namespace moonstone::syren {
struct RuntimeEvent{std::string category,message;SourceSpan source;std::uint64_t thread=0,object=0;};
class RuntimeRecorder {
 std::size_t capacity_;mutable std::mutex mu_;std::deque<RuntimeEvent> events_;
public:
 explicit RuntimeRecorder(std::size_t capacity=1024):capacity_(capacity){if(!capacity)throw std::invalid_argument("capacity must be positive");}
 void record(RuntimeEvent event){std::lock_guard<std::mutex> lock(mu_);if(events_.size()==capacity_)events_.pop_front();events_.push_back(std::move(event));}
 std::vector<RuntimeEvent> snapshot()const{std::lock_guard<std::mutex> lock(mu_);return {events_.begin(),events_.end()};}
};
}
