#pragma once

#include <pawndb/worker_pool.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <unordered_map>

namespace pawndb {

class Lifecycle {
 public:
  using Context = std::weak_ptr<void>;

  void start() noexcept {
    running_ = true;
    main_thread_ = std::this_thread::get_id();
  }
  void stop() {
    scripts_.clear();
    if (pool_) pool_->stop();
    pool_.reset();
    running_ = false;
  }
  bool attach(void* amx) {
    return running_ && amx && scripts_.emplace(amx, std::make_shared<int>(0)).second;
  }
  void detach(void* amx) { scripts_.erase(amx); }
  Context context(void* amx) const {
    auto it = scripts_.find(amx);
    return it == scripts_.end() ? Context{} : it->second;
  }
  static Work guard_callback(Context context, Work callback) {
    return [context = std::move(context), callback = std::move(callback)] {
      if (context.lock()) callback();
    };
  }
  WorkerPool* worker_pool() {
    if (!running_) return nullptr;
    if (!pool_) pool_ = std::make_unique<WorkerPool>();
    return pool_.get();
  }
  void dispatch_tick() {
    if (!running_ || !pool_ || std::this_thread::get_id() != main_thread_) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    for (int n = 0; n < 64 && std::chrono::steady_clock::now() < deadline; ++n) {
      auto result = pool_->take_completed();
      if (!result) break;
      result();
    }
  }
  std::size_t script_count() const noexcept { return scripts_.size(); }

 private:
  std::unordered_map<void*, std::shared_ptr<void>> scripts_;
  std::unique_ptr<WorkerPool> pool_;
  std::thread::id main_thread_;
  bool running_ = false;
};

}  // namespace pawndb
