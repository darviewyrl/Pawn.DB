#include <pawndb/connection_manager.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

int main() {
  pawndb::Lifecycle life;
  life.start();
  int script = 0;
  life.attach(&script);
  std::atomic<int> executions{0};
  struct Pool final : pawndb::SessionPool {
    std::atomic<int>& executions;
    explicit Pool(std::atomic<int>& count) : executions(count) {}
    bool query(std::string_view, pawndb::DriverError&) override {
      std::this_thread::sleep_for(2ms);
      ++executions;
      return true;
    }
  };
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
      [](std::string) {}, [&](const auto&, auto&) { return std::make_shared<Pool>(executions); });
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
  for (int i = 0; i < 4; ++i)
    if (!manager.query(handle, "SELECT 1", [&](bool ok, auto) { if (ok) ++completed; })) return 1;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (completed != 4 && std::chrono::steady_clock::now() < deadline) {
    life.dispatch_tick();
    std::this_thread::yield();
  }
  const auto metrics = manager.metrics(handle);
  if (completed != 4 || executions != 4 || !metrics || metrics->qps != 4 || metrics->pending ||
      metrics->average_latency_us < 1000 || metrics->slow_queries != 4 ||
      !manager.set_slow_query_threshold(handle, 0)) return 1;

  if (!manager.query(handle, "SELECT 1", [&](bool ok, auto) { if (ok) ++completed; })) return 1;
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
