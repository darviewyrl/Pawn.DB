#include <pawndb/mariadb_pool.hpp>
#include <pawndb/lifecycle.hpp>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  pawndb::ConnectionConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.pool_size = 2;
  config.connect_timeout = 1;
  config.multi_statements = true;
  const bool online = argc == 6;
  if (online) {
    config.port = std::atoi(argv[1]);
    config.user = argv[2];
    config.password = argv[3];
    config.database = argv[4];
    config.host = argv[5];
  } else if (argc != 1) return 1;

  pawndb::Lifecycle life;
  life.start();
  int amx = 0;
  life.attach(&amx);
  auto context = life.context(&amx);
  auto* worker = life.worker_pool();
  bool done = false, passed = false, tick_slow = false;
  if (!worker->submit(0, pawndb::WorkerPool::Priority::normal, [&, worker] {
        pawndb::DriverError error;
        auto sessions = pawndb::MariaPool::open(config, error);
        if (online && sessions) {
          pawndb::DriverError query_error;
          pawndb::QueryResult rows;
          const std::string input = "ไทย'; SELECT 1; --\\";
          std::vector<char> escaped(input.size() * 2 + 1);
          std::size_t escaped_size = 0;
          passed = sessions->escape_string(input, escaped, escaped_size) &&
                   std::string_view(escaped.data(), escaped_size).starts_with("ไทย") &&
                   sessions->query("SELECT '" + std::string(escaped.data(), escaped_size) + "'",
                                   query_error) &&
                   sessions->query_result("SELECT 1 AS value", query_error, rows) &&
                   rows.fields == std::vector<std::string>{"value"} && rows.rows.size() == 1 &&
                   rows.rows[0][0] == std::optional<std::string>{"1"} &&
                   sessions->query_result("SELECT 1 AS first; SELECT 2 AS second", query_error, rows) &&
                   rows.fields == std::vector<std::string>{"first"} &&
                   rows.next_results.size() == 1 &&
                   rows.next_results[0].fields == std::vector<std::string>{"second"} &&
                   rows.next_results[0].rows[0][0] == std::optional<std::string>{"2"} &&
                   sessions->query("SELECT 3", query_error) &&
                   sessions->query("SELECT SLEEP(0.2)", query_error) &&
                   !sessions->query("INVALID SQL", query_error) && query_error.code > 0 &&
                   sessions->query("SELECT 1", query_error);
        } else if (!online) {
          passed = !sessions && error.code > 0 && !error.message.empty();
        }
        sessions.reset();
        worker->publish(pawndb::Lifecycle::guard_callback(context, [&] { done = true; }));
      })) return 1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!done && std::chrono::steady_clock::now() < deadline) {
    const auto start = std::chrono::steady_clock::now();
    life.dispatch_tick();
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(50)) tick_slow = true;
    std::this_thread::yield();
  }
  life.stop();
  return done && passed && !tick_slow ? 0 : 1;
}
