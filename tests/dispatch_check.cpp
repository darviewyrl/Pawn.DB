#include <pawndb/lifecycle.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

int main() {
  pawndb::Lifecycle life;
  life.start();
  int script = 0;
  if (!life.attach(&script)) return 1;
  auto* pool = life.worker_pool();
  auto context = life.context(&script);
  const auto main_thread = std::this_thread::get_id();
  std::atomic<int> published{0};
  int called = 0;
  bool wrong_thread = false;
  for (int i = 0; i < 1000; ++i) {
    auto callback = life.guard_callback(context, [&] {
      wrong_thread |= std::this_thread::get_id() != main_thread;
      ++called;
    });
    if (!pool->submit(i % pool->size(), pawndb::WorkerPool::Priority::high,
                      [&, callback = std::move(callback)] {
                        pool->publish(callback);
                        ++published;
                      })) return 1;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (published != 1000) {
    if (std::chrono::steady_clock::now() > deadline) return 1;
    std::this_thread::yield();
  }
  std::thread wrong([&] { life.dispatch_tick(); });
  wrong.join();
  if (called) return 1;
  while (called < 1000) {
    const auto before = called;
    life.dispatch_tick();
    if (called - before > 64 || std::chrono::steady_clock::now() > deadline) return 1;
  }
  if (wrong_thread) return 1;

  pool->publish(life.guard_callback(context, [&] {
    ++called;
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }));
  pool->publish(life.guard_callback(context, [&] { ++called; }));
  life.dispatch_tick();
  if (called != 1001) return 1;
  life.dispatch_tick();
  if (called != 1002) return 1;

  auto payload = std::make_shared<int>(42);
  std::weak_ptr<int> released = payload;
  pool->publish(life.guard_callback(context, [payload, &called] { ++called; }));
  payload.reset();
  life.detach(&script);
  if (!life.attach(&script)) return 1;
  pool->publish(life.guard_callback(life.context(&script), [&] { ++called; }));
  life.dispatch_tick();
  if (called != 1003 || !released.expired()) return 1;
  payload = std::make_shared<int>(43);
  released = payload;
  pool->publish(life.guard_callback(life.context(&script), [payload, &called] { ++called; }));
  payload.reset();
  life.stop();
  return life.script_count() == 0 && released.expired() && called == 1003 ? 0 : 1;
}
