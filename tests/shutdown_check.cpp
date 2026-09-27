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
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false, release = false;
  std::atomic<int> executed{0};
  struct Pool final : pawndb::SessionPool {
    std::mutex& mutex;
    std::condition_variable& ready;
    bool& entered;
    bool& release;
    std::atomic<int>& executed;
    Pool(std::mutex& lock, std::condition_variable& condition, bool& started,
         bool& go, std::atomic<int>& count)
        : mutex(lock), ready(condition), entered(started), release(go), executed(count) {}
    bool query(std::string_view, pawndb::DriverError&) override {
      if (!executed.load()) {
        std::unique_lock lock(mutex);
        entered = true;
        ready.notify_all();
        ready.wait(lock, [&] { return release; });
      }
      ++executed;
      return true;
    }
  };

  std::size_t drained = 0;
  {
    pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
        [](std::string) {}, [&](const auto&, auto&) {
          return std::make_shared<Pool>(mutex, ready, entered, release, executed);
        });
    pawndb::ConnectionConfig config;
    config.host = "localhost";
    config.user = "test";
    config.database = "test";
    const auto handle = manager.connect(&script, config);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!manager.is_connected(handle) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    if (!handle || !manager.is_connected(handle)) return 1;
    if (!manager.query(handle, "INSERT 0", {}, pawndb::WorkerPool::Priority::normal, true))
      return 1;
    {
      std::unique_lock lock(mutex);
      if (!ready.wait_for(lock, 2s, [&] { return entered; })) return 1;
    }
    for (int i = 1; i < 500; ++i)
      if (!manager.query(handle, "INSERT " + std::to_string(i), {},
                          pawndb::WorkerPool::Priority::normal, true)) return 1;
    std::thread unblock([&] {
      std::this_thread::sleep_for(20ms);
      { std::lock_guard lock(mutex); release = true; }
      ready.notify_all();
    });
    drained = manager.shutdown();
    unblock.join();
    if (manager.query(handle, "INSERT rejected", {}, pawndb::WorkerPool::Priority::normal, true) ||
        !manager.close(handle)) return 1;
  }
  life.stop();
  return drained == 500 && executed == 500 ? 0 : 1;
}
