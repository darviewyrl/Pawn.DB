#include <pawndb/worker_pool.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

int main() {
  std::array<int, 3> order{};
  int completed = 0;
  {
    pawndb::WorkerPool pool(3);
    for (std::size_t worker = 0; worker < 3; ++worker)
      for (int i = 0; i < 2000; ++i)
        if (!pool.submit(worker, pawndb::WorkerPool::Priority::normal, [&, worker, i] {
              if (order[worker]++ != i) order[worker] = -10000;
              pool.publish([&] { ++completed; });
            })) return 1;
    pool.stop();
    while (auto result = pool.take_completed()) result();
  }
  if (completed != 6000 || order != std::array<int, 3>{2000, 2000, 2000}) return 1;

  std::atomic<bool> started{false}, release{false};
  std::vector<int> execution;
  {
    pawndb::WorkerPool pool(1);
    if (!pool.submit(0, pawndb::WorkerPool::Priority::high, [&] {
          started.store(true);
          while (!release.load()) std::this_thread::yield();
        })) return 1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!started.load()) {
      if (std::chrono::steady_clock::now() > deadline) {
        release.store(true);
        return 1;
      }
      std::this_thread::yield();
    }
    for (int i = 0; i < 10; ++i) {
      if (!pool.submit(0, pawndb::WorkerPool::Priority::normal,
                       [&, i] { execution.push_back(100 + i); })) {
        release.store(true);
        return 1;
      }
      if (!pool.submit(0, pawndb::WorkerPool::Priority::high,
                       [&, i] { execution.push_back(i); })) {
        release.store(true);
        return 1;
      }
    }
    release.store(true);
    pool.stop();
  }
  if (execution.size() != 20) return 1;
  for (int i = 0; i < 10; ++i)
    if (execution[i] != i || execution[10 + i] != 100 + i) return 1;
}
