#include <pawndb/connection_manager.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <string>
#include <utility>

using namespace std::chrono_literals;

int main() {
  pawndb::Lifecycle life;
  life.start();
  int script = 0;
  if (!life.attach(&script)) return 1;
  struct Pool final : pawndb::SessionPool {
    bool query(std::string_view, pawndb::DriverError&) override { return true; }
    bool query_result(std::string_view, pawndb::DriverError&,
                      pawndb::QueryResult& result) override {
      result.fields = {"value"};
      result.rows = {{std::string("row")}};
      return true;
    }
  };
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
      [](std::string) {}, [](const auto&, auto&) { return std::make_shared<Pool>(); });
  pawndb::QueryResult data;
  data.fields = {"value"};
  data.rows = {{std::string("row")}};

  pawndb::ConnectionConfig config;
  config.host = "localhost";
  config.user = "test";
  config.database = "test";
  const auto connection = manager.connect(&script, config);
  const auto connect_deadline = std::chrono::steady_clock::now() + 3s;
  while (!manager.is_connected(connection) && std::chrono::steady_clock::now() < connect_deadline)
    std::this_thread::yield();
  if (!connection || !manager.is_connected(connection)) return 1;

  int completed = 0;
  bool failed = false;
  for (int submitted = 0; submitted < 50000;) {
    const auto batch_end = std::min(50000, submitted + 1000);
    while (submitted < batch_end) {
      if (manager.query(connection, "SELECT 1", [&](bool ok, auto, pawndb::QueryResult result) {
            const auto handle = manager.create_result(&script, std::move(result));
            const auto object = manager.result(&script, handle);
            if (!ok || !object || object->cursor != -1 || object->data.rows.size() != 1)
              failed = true;
            manager.release_scoped_result(&script, handle);
            ++completed;
          }, pawndb::WorkerPool::Priority::high, false, true)) {
        ++submitted;
      } else {
        life.dispatch_tick();
        std::this_thread::yield();
      }
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (completed < submitted && std::chrono::steady_clock::now() < deadline) {
      life.dispatch_tick();
      std::this_thread::yield();
    }
    if (completed != submitted || failed) return 1;
  }

  const auto retained = manager.create_result(&script, data);
  const auto result = manager.result(&script, retained);
  if (!result || result->cursor != -1 || !manager.retain_result(&script, retained)) return 1;
  manager.release_scoped_result(&script, retained);
  if (!manager.result(&script, retained) || !manager.free_result(&script, retained) ||
      manager.result(&script, retained)) return 1;

  const auto unloaded = manager.create_result(&script, data);
  if (!unloaded || !manager.retain_result(&script, unloaded)) return 1;
  manager.detach(&script);
  if (manager.result(&script, unloaded)) return 1;
  life.detach(&script);
  life.stop();
}
