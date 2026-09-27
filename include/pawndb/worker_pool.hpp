#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pawndb {

using Work = std::function<void()>;

// One producer (the game thread), one consumer (one worker).
template <std::size_t Capacity = 8192>
class SpscQueue {
 public:
  bool push(Work work) {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == Capacity) return false;
    slots_[tail % Capacity] = std::move(work);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  Work pop() {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return {};
    Work work = std::move(slots_[head % Capacity]);
    head_.store(head + 1, std::memory_order_release);
    return work;
  }

 private:
  std::array<Work, Capacity> slots_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

// Many workers publish; only the game thread consumes.
class MpscQueue {
 public:
  MpscQueue() = default;
  MpscQueue(const MpscQueue&) = delete;
  MpscQueue& operator=(const MpscQueue&) = delete;
  ~MpscQueue() {
    while (pop()) {}
  }

  void push(Work work) {
    auto* node = new Node{std::move(work), nullptr};
    node->next = head_.load(std::memory_order_relaxed);
    while (!head_.compare_exchange_weak(node->next, node, std::memory_order_release,
                                        std::memory_order_relaxed)) {}
  }

  Work pop() {
    if (!ready_) {
      auto* batch = head_.exchange(nullptr, std::memory_order_acquire);
      while (batch) {
        auto* next = batch->next;
        batch->next = ready_;
        ready_ = batch;
        batch = next;
      }
    }
    if (!ready_) return {};
    auto* node = std::exchange(ready_, ready_->next);
    Work work = std::move(node->work);
    delete node;
    return work;
  }

 private:
  struct Node {
    Work work;
    Node* next;
  };
  alignas(64) std::atomic<Node*> head_{nullptr};
  alignas(64) Node* ready_ = nullptr;
};

class WorkerPool {
 public:
  enum class Priority { high, normal };

  explicit WorkerPool(std::size_t count = 0) {
    if (!count) count = std::clamp(std::thread::hardware_concurrency(), 2u, 4u);
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) workers_.push_back(std::make_unique<Worker>());
    try {
      for (auto& worker : workers_) threads_.emplace_back([this, p = worker.get()] { run(*p); });
      timer_ = std::thread([this] { run_timers(); });
    } catch (...) {
      stop();
      throw;
    }
  }

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  ~WorkerPool() { stop(); }

  std::size_t size() const { return workers_.size(); }

  bool submit(std::size_t index, Priority priority, Work work) {
    if (stopping_.load(std::memory_order_acquire) || index >= workers_.size() || !work)
      return false;
    auto& worker = *workers_[index];
    auto& queue = priority == Priority::high ? worker.high : worker.normal;
    if (!queue.push(std::move(work))) return false;
    worker.wake.fetch_add(1, std::memory_order_release);
    worker.wake.notify_one();
    return true;
  }

  bool schedule(std::size_t index, std::chrono::steady_clock::duration delay, Work work) {
    if (stopping_.load(std::memory_order_acquire) || index >= workers_.size() || !work)
      return false;
    {
      std::lock_guard lock(timer_mutex_);
      if (stopping_.load(std::memory_order_acquire)) return false;
      timers_.emplace(std::chrono::steady_clock::now() + delay, Timer{index, std::move(work)});
    }
    timer_ready_.notify_one();
    return true;
  }

  void defer_cleanup(std::size_t index, Work work) {
    auto& worker = *workers_[index];
    worker.cleanup.push(std::move(work));
    worker.wake.fetch_add(1, std::memory_order_release);
    worker.wake.notify_one();
  }

  void publish(Work result) {
    if (result) completed_.push(std::move(result));
  }
  Work take_completed() { return completed_.pop(); }

  void stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
    timer_ready_.notify_one();
    if (timer_.joinable()) timer_.join();
    for (auto& worker : workers_) {
      worker->wake.fetch_add(1, std::memory_order_release);
      worker->wake.notify_one();
    }
    for (auto& thread : threads_) thread.join();
  }

 private:
  struct Worker {
    SpscQueue<> high;
    SpscQueue<> normal;
    MpscQueue cleanup;
    MpscQueue delayed;
    std::atomic<unsigned> wake{0};
  };

  void run(Worker& worker) {
    for (;;) {
      const auto wake = worker.wake.load(std::memory_order_acquire);
      if (auto work = worker.high.pop()) { work(); continue; }
      if (auto work = worker.delayed.pop()) { work(); continue; }
      if (auto work = worker.normal.pop()) { work(); continue; }
      if (auto work = worker.cleanup.pop()) { work(); continue; }
      if (stopping_.load(std::memory_order_acquire)) break;
      worker.wake.wait(wake, std::memory_order_acquire);
    }
  }

  struct Timer {
    std::size_t worker;
    Work work;
  };

  void run_timers() {
    std::unique_lock lock(timer_mutex_);
    while (!stopping_.load(std::memory_order_acquire)) {
      if (timers_.empty()) {
        timer_ready_.wait(lock, [this] {
          return stopping_.load(std::memory_order_acquire) || !timers_.empty();
        });
        continue;
      }
      const auto due = timers_.begin()->first;
      if (timer_ready_.wait_until(lock, due) != std::cv_status::timeout) continue;
      const auto now = std::chrono::steady_clock::now();
      while (!timers_.empty() && timers_.begin()->first <= now) {
        auto timer = std::move(timers_.begin()->second);
        timers_.erase(timers_.begin());
        auto& worker = *workers_[timer.worker];
        worker.delayed.push(std::move(timer.work));
        worker.wake.fetch_add(1, std::memory_order_release);
        worker.wake.notify_one();
      }
    }
  }

  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::thread> threads_;
  std::thread timer_;
  std::mutex timer_mutex_;
  std::condition_variable timer_ready_;
  std::multimap<std::chrono::steady_clock::time_point, Timer> timers_;
  MpscQueue completed_;
  std::atomic<bool> stopping_{false};
};

}  // namespace pawndb
