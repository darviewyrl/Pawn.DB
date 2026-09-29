#include <pawndb/connection_manager.hpp>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

int main() {
  pawndb::Lifecycle life;
  life.start();
  int script = 0;
  if (!life.attach(&script)) return 1;
  struct Pool final : pawndb::SessionPool {
    bool query(std::string_view, pawndb::DriverError&) override { return true; }
    bool query_result(std::string_view sql, pawndb::DriverError& error,
                      pawndb::QueryResult& result) override {
      if (sql == "bad") { error = {1062, "duplicate key"}; return false; }
      result.fields = {"sql"};
      result.rows = {{std::string(sql)}};
      return true;
    }
  };
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
      [](std::string) {}, [](const auto&, auto&) { return std::make_shared<Pool>(); });
  pawndb::ConnectionConfig config;
  config.host = "localhost";
  config.user = "test";
  config.database = "test";
  const auto connection = manager.connect(&script, config);
  const auto connect_deadline = std::chrono::steady_clock::now() + 3s;
  while (!manager.is_connected(connection) && std::chrono::steady_clock::now() < connect_deadline)
    std::this_thread::yield();
  if (!manager.is_connected(connection)) return 2;

  const auto batch = manager.create_batch(&script, connection, false);
  if (!batch || !manager.add_batch(&script, batch, "one") ||
      !manager.add_batch(&script, batch, "bad") || !manager.add_batch(&script, batch, "four"))
    return 3;
  bool completed = false, valid = false;
  if (!manager.submit_batch(&script, batch, [&](pawndb::BatchExecutionResult execution) {
        const auto result_handle = manager.create_batch_result(&script, std::move(execution));
        auto result = manager.batch_result(&script, result_handle);
        if (result && result->items.size() == 3 &&
            result->items[0].status == pawndb::BatchItemStatus::success &&
            result->items[1].status == pawndb::BatchItemStatus::sql_error &&
            result->items[1].error_message == "duplicate key" &&
            result->items[2].status == pawndb::BatchItemStatus::success &&
            result->items[0].result_handle && result->items[2].result_handle &&
            manager.result(&script, result->items[0].result_handle) &&
            manager.retain_batch_result(&script, result_handle)) {
          manager.release_scoped_batch_result(&script, result_handle);
          valid = manager.batch_result(&script, result_handle) != nullptr;
          valid = valid && manager.free_batch_result(&script, result_handle);
          valid = valid && !manager.result(&script, result->items[0].result_handle);
        }
        completed = true;
      })) return 4;
  if (manager.batch_connection(&script, batch)) return 5;
  const auto batch_deadline = std::chrono::steady_clock::now() + 3s;
  while (!completed && std::chrono::steady_clock::now() < batch_deadline) {
    life.dispatch_tick();
    std::this_thread::yield();
  }
  if (!completed || !valid) return 6;

  pawndb::BatchExecutionResult scoped;
  scoped.items.resize(1);
  scoped.items[0].status = pawndb::BatchItemStatus::success;
  scoped.items[0].result.fields = {"value"};
  const auto scoped_handle = manager.create_batch_result(&script, std::move(scoped));
  auto scoped_result = manager.batch_result(&script, scoped_handle);
  const auto child = scoped_result->items[0].result_handle;
  if (!manager.retain_result(&script, child)) return 7;
  manager.release_scoped_batch_result(&script, scoped_handle);
  if (manager.batch_result(&script, scoped_handle) || !manager.result(&script, child) ||
      !manager.free_result(&script, child)) return 8;

  pawndb::BatchExecutionResult unload;
  unload.items.resize(1);
  unload.items[0].status = pawndb::BatchItemStatus::success;
  const auto unload_handle = manager.create_batch_result(&script, std::move(unload));
  auto unload_result = manager.batch_result(&script, unload_handle);
  const auto unload_child = unload_result->items[0].result_handle;
  if (!manager.retain_batch_result(&script, unload_handle) || !manager.close(connection)) return 9;
  manager.detach(&script);
  if (manager.batch_result(&script, unload_handle) || manager.result(&script, unload_child)) return 10;
  life.detach(&script);
  life.stop();
}
