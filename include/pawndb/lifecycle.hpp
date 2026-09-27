#pragma once

#include <cstddef>
#include <unordered_set>

namespace pawndb {

class Lifecycle {
 public:
  void start() noexcept { running_ = true; }
  void stop() noexcept {
    running_ = false;
    scripts_.clear();
  }
  bool attach(void* amx) { return running_ && amx && scripts_.insert(amx).second; }
  void detach(void* amx) { scripts_.erase(amx); }
  void dispatch_tick() noexcept {}
  std::size_t script_count() const noexcept { return scripts_.size(); }

 private:
  std::unordered_set<void*> scripts_;
  bool running_ = false;
};

}  // namespace pawndb
