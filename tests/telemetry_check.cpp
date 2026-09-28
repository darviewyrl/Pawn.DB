#include <pawndb/connection_manager.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

int main() {
  pawndb::Lifecycle life;
  life.start();
  int script = 0;
  life.attach(&script);
  std::atomic<int> executions{0};
  std::atomic<int> started{0};
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false, release = false;
  struct Pool final : pawndb::SessionPool {
    std::atomic<int>& executions;
    std::atomic<int>& started;
    std::mutex& mutex;
    std::condition_variable& ready;
    bool& entered;
    bool& release;
    Pool(std::atomic<int>& count, std::atomic<int>& begins, std::mutex& lock,
         std::condition_variable& condition, bool& waiting, bool& go)
        : executions(count), started(begins), mutex(lock), ready(condition),
          entered(waiting), release(go) {}
    bool query(std::string_view, pawndb::DriverError&) override {
      if (++started == 1) {
        std::unique_lock lock(mutex);
        entered = true;
        ready.notify_all();
        ready.wait(lock, [&] { return release; });
      }
      std::this_thread::sleep_for(2ms);
      ++executions;
      return true;
    }
  };
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
      [](std::string) {}, [&](const auto&, auto&) {
        return std::make_shared<Pool>(executions, started, mutex, ready, entered, release);
      });
  pawndb::ConnectionConfig config;
  config.host = "localhost";
  config.user = "test";
  config.database = "test";
  const auto handle = manager.connect(&script, config);
  const auto connected = std::chrono::steady_clock::now() + 2s;
  while (!manager.is_connected(handle) && std::chrono::steady_clock::now() < connected)
    std::this_thread::yield();
  if (!handle || !manager.is_connected(handle) || !manager.set_slow_query_threshold(handle, 1000))
    return 1;

  std::atomic<int> completed{0};
  if (!manager.query(handle, "SELECT 1", [&](bool ok, auto, auto) { if (ok) ++completed; })) return 1;
  {
    std::unique_lock lock(mutex);
    if (!ready.wait_for(lock, 2s, [&] { return entered; })) return 1;
  }
  for (int i = 1; i < 4; ++i)
    if (!manager.query(handle, "SELECT 1", [&](bool ok, auto, auto) { if (ok) ++completed; })) return 1;
  const auto queued = manager.metrics(handle);
  if (!queued || queued->pending != 3) return 1;
  { std::lock_guard lock(mutex); release = true; }
  ready.notify_all();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (completed != 4 && std::chrono::steady_clock::now() < deadline) {
    life.dispatch_tick();
    std::this_thread::yield();
  }
  const auto metrics = manager.metrics(handle);
  if (completed != 4 || executions != 4 || !metrics || metrics->qps != 4 || metrics->pending ||
      metrics->average_latency_us < 1000 || metrics->slow_queries != 4 ||
      !manager.set_slow_query_threshold(handle, 0)) return 1;

  if (!manager.query(handle, "SELECT 1", [&](bool ok, auto, auto) { if (ok) ++completed; })) return 1;
  const auto final_deadline = std::chrono::steady_clock::now() + 2s;
  while (completed != 5 && std::chrono::steady_clock::now() < final_deadline) {
    life.dispatch_tick();
    std::this_thread::yield();
  }
  const auto final = manager.metrics(handle);
  if (completed != 5 || !final || final->qps != 5 || final->slow_queries != 4 ||
      !manager.is_connected(handle)) return 1;
  std::this_thread::sleep_for(1100ms);
  const auto expired = manager.metrics(handle);
  if (!expired || expired->qps != 0 || !manager.close(handle)) return 1;
  life.stop();
  return 0;
}
