#include <pawndb/connection_manager.hpp>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

int main() {
  if (pawndb::next_reconnect_delay(std::chrono::seconds(1)) != std::chrono::seconds(2) ||
      pawndb::next_reconnect_delay(std::chrono::seconds(2)) != std::chrono::seconds(4) ||
      pawndb::next_reconnect_delay(std::chrono::seconds(4)) != std::chrono::seconds(8) ||
      pawndb::next_reconnect_delay(std::chrono::seconds(8)) != std::chrono::seconds(15) ||
      pawndb::next_reconnect_delay(std::chrono::seconds(15)) != std::chrono::seconds(15)) return 1;
  pawndb::Lifecycle life;
  life.start();
  int script = 0, other = 0;
  life.attach(&script);
  life.attach(&other);
  int errors = 0, warnings = 0;
  pawndb::ConnectionManager manager(life,
      [&](void* amx, auto, int code, std::string) { if (amx == &script && code == -1) ++errors; },
      [&](std::string) { ++warnings; });
  pawndb::ConnectionConfig direct;
  auto setup = manager.setup_init(&script);
  if (!setup || !manager.setup_charset(setup, "latin1") ||
      !manager.setup_option(setup, 0, 8)) return 1;
  auto handle = manager.connect(&script, direct, setup, false);
  life.dispatch_tick();
  if (!manager.setup_charset(setup, "utf8mb4")) return 1;
  if (!handle || manager.is_connected(handle) || !manager.driver_name(handle) ||
      !manager.set_debug_level(handle, 2) || !manager.free_setup(setup)) return 1;
  if (!manager.close(handle) || manager.close(handle) || manager.is_connected(handle)) return 1;

  const auto path = std::filesystem::current_path() / "pdb_missing_config.json";
  std::filesystem::remove(path);
  handle = manager.connect_file(&script, path.string());
  if (!handle || manager.is_connected(handle)) return 1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!errors && std::chrono::steady_clock::now() < deadline) {
    life.dispatch_tick();
    std::this_thread::yield();
  }
  if (errors != 1 || !std::filesystem::exists(path) || !manager.close(handle)) return 1;
  std::filesystem::remove(path);
  handle = manager.connect(&other, direct);
  manager.detach(&other);
  life.detach(&other);
  if (manager.is_connected(handle) || warnings < 2) return 1;
  std::atomic<bool> released = false;
  std::thread::id released_thread;
  struct FakePool final : pawndb::SessionPool {
    std::atomic<bool>& released;
    std::thread::id& released_thread;
    FakePool(std::atomic<bool>& value, std::thread::id& thread)
        : released(value), released_thread(thread) {}
    ~FakePool() override { released_thread = std::this_thread::get_id(); released = true; }
    bool query(std::string_view, pawndb::DriverError&) override { return true; }
  };
  {
    pawndb::ConnectionManager live(life, [](void*, auto, int, std::string) {},
                                   [](std::string) {},
                                   [&](const pawndb::ConnectionConfig&, pawndb::DriverError&)
                                       -> std::shared_ptr<pawndb::SessionPool> {
                                     return std::make_shared<FakePool>(released, released_thread);
                                   });
    handle = live.connect(&script, direct);
    const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!live.is_connected(handle) && std::chrono::steady_clock::now() < connect_deadline)
      std::this_thread::yield();
    if (!live.is_connected(handle) || !live.close(handle)) return 1;
    const auto valid = std::filesystem::current_path() / "pdb_valid_config.json";
    { std::ofstream file(valid); file << pawndb::default_config_json(); }
    handle = live.connect_file(&script, valid.string());
    const auto file_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!live.is_connected(handle) && std::chrono::steady_clock::now() < file_deadline)
      std::this_thread::yield();
    std::filesystem::remove(valid);
    if (!live.is_connected(handle) || !live.close(handle)) return 1;
    auto postgres = direct;
    postgres.backend = pawndb::Backend::postgres;
    postgres.port = 5432;
    handle = live.connect(&script, postgres);
    const auto pg_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!live.is_connected(handle) && std::chrono::steady_clock::now() < pg_deadline)
      std::this_thread::yield();
    if (!live.is_connected(handle) || live.driver_name(handle) != "PostgreSQL" ||
        !live.close(handle)) return 1;
  }
  int driver_errors = 0;
  std::vector<pawndb::ConnectionConfig> captured;
  std::mutex captured_mutex;
  {
    pawndb::ConnectionManager failed(life,
        [&](void* amx, auto, int code, std::string message) {
          if (amx == &script && code == 1045 && message == "access denied") ++driver_errors;
        }, [](std::string) {},
        [&](const pawndb::ConnectionConfig& config, pawndb::DriverError& error)
            -> std::shared_ptr<pawndb::SessionPool> {
          { std::lock_guard lock(captured_mutex); captured.push_back(config); }
          error = {1045, "access denied"};
          return {};
        });
    handle = failed.connect(&script, direct);
    const auto error_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!driver_errors && std::chrono::steady_clock::now() < error_deadline) {
      life.dispatch_tick();
      std::this_thread::yield();
    }
    if (driver_errors != 1 || failed.is_connected(handle) || !failed.close(handle)) return 1;
    const auto setup = failed.setup_init(&script);
    if (!setup || !failed.setup_charset(setup, "latin1") ||
        !failed.setup_option(setup, 0, 12) || !failed.setup_option(setup, 1, 0) ||
        !failed.setup_option(setup, 2, 1) || !failed.setup_option(setup, 3, 2) ||
        !failed.setup_driver(setup, 2) || failed.setup_ssl(setup, "ca.pem", "client.pem", "", true) ||
        !failed.setup_ssl(setup, "ca.pem", "client.pem", "client.key", true)) return 1;
    auto routed = direct;
    routed.port = 33306;
    handle = failed.connect(&script, routed, setup, false);
    if (!handle || !failed.setup_charset(setup, "utf8mb4")) return 1;
    handle = failed.connect(&script, routed, setup, false);
    if (!handle || !failed.free_setup(setup)) return 1;
    const auto reuse_setup = failed.setup_init(&script);
    if (!reuse_setup) return 1;
    handle = failed.connect(&script, routed, reuse_setup, true);
    if (!handle || failed.setup_charset(reuse_setup, "utf8mb4") ||
        failed.free_setup(reuse_setup)) return 1;
    const auto manual_setup = failed.setup_init(&script);
    if (!manual_setup || !failed.free_setup(manual_setup) || failed.free_setup(manual_setup)) return 1;
    const auto unload_setup = failed.setup_init(&other);
    if (!unload_setup) return 1;
    failed.detach(&other);
    if (failed.setup_charset(unload_setup, "latin1")) return 1;
    const auto capture_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < capture_deadline) {
      { std::lock_guard lock(captured_mutex); if (captured.size() == 4) break; }
      std::this_thread::yield();
    }
    {
      std::lock_guard lock(captured_mutex);
      if (captured.size() != 4 || captured[1].backend != pawndb::Backend::postgres ||
          captured[1].port != 33306 || captured[1].charset != "latin1" ||
          captured[1].connect_timeout != 12 || captured[1].auto_reconnect ||
          !captured[1].multi_statements || captured[1].pool_size != 2 ||
          !captured[1].ssl_enabled || captured[1].ca_cert != "ca.pem" ||
          captured[2].charset != "utf8mb4" || captured[3].ssl_enabled ||
          captured[3].backend != pawndb::Backend::mariadb) return 1;
    }
  }
  {
    auto online = std::make_shared<std::atomic<bool>>(true);
    std::atomic<int> queries{0};
    std::vector<std::string> statements;
    std::mutex statements_mutex;
    struct NetworkPool final : pawndb::SessionPool {
      std::shared_ptr<std::atomic<bool>> online;
      std::atomic<int>& queries;
      std::vector<std::string>& statements;
      std::mutex& statements_mutex;
      std::mutex& release_mutex;
      std::vector<std::thread::id>& released_threads;
      NetworkPool(std::shared_ptr<std::atomic<bool>> state, std::atomic<int>& count,
                  std::vector<std::string>& sql, std::mutex& sql_mutex,
                  std::mutex& release_lock, std::vector<std::thread::id>& release_threads)
          : online(std::move(state)), queries(count), statements(sql),
            statements_mutex(sql_mutex), release_mutex(release_lock),
            released_threads(release_threads) {}
      ~NetworkPool() override {
        std::lock_guard lock(release_mutex);
        released_threads.push_back(std::this_thread::get_id());
      }
      bool query(std::string_view sql, pawndb::DriverError& error) override {
        if (!online->load()) { error = {-4, "offline"}; return false; }
        ++queries;
        { std::lock_guard lock(statements_mutex); statements.emplace_back(sql); }
        return true;
      }
      bool ping(pawndb::DriverError& error) override {
        if (online->load()) return true;
        error = {-4, "offline"};
        return false;
      }
    };
    std::mutex release_mutex;
    std::vector<std::thread::id> released_escape_threads;
    pawndb::ConnectionManager reconnecting(life,
        [](void*, auto, int, std::string) {}, [](std::string) {},
        [online, &queries, &statements, &statements_mutex, &release_mutex,
         &released_escape_threads](
            const pawndb::ConnectionConfig&, pawndb::DriverError& error)
            -> std::shared_ptr<pawndb::SessionPool> {
          if (!online->load()) { error = {-4, "offline"}; return {}; }
          return std::make_shared<NetworkPool>(online, queries, statements, statements_mutex,
                                               release_mutex, released_escape_threads);
        });
    const auto recovered = reconnecting.connect(&script, direct);
    const auto connected_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!reconnecting.is_connected(recovered) &&
           std::chrono::steady_clock::now() < connected_deadline) std::this_thread::yield();
    if (!reconnecting.is_connected(recovered)) return 1;
    auto old_escape = reconnecting.escape_snapshot(recovered);
    if (!old_escape) return 1;
    online->store(false);
    const auto disconnected_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (reconnecting.is_connected(recovered) &&
           std::chrono::steady_clock::now() < disconnected_deadline) std::this_thread::yield();
    if (reconnecting.is_connected(recovered)) return 1;
    if (!reconnecting.query(recovered, "SELECT retained")) return 1;
    std::atomic<bool> query_done{false}, query_ok{false};
    if (!reconnecting.query(recovered, "SELECT retained callback", [&](bool ok, auto, auto) {
          query_ok = ok;
          query_done = true;
        })) return 1;
    online->store(true);
    const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!query_done && std::chrono::steady_clock::now() < recovery_deadline) {
      life.dispatch_tick();
      std::this_thread::yield();
    }
    auto new_escape = reconnecting.escape_snapshot(recovered);
    if (!query_done || !query_ok || !reconnecting.is_connected(recovered) || queries != 2 ||
        !new_escape || new_escape == old_escape) return 1;
    new_escape.reset();
    if (!reconnecting.close(recovered)) return 1;
    old_escape.reset();
    const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < cleanup_deadline) {
      { std::lock_guard lock(release_mutex); if (released_escape_threads.size() == 2) break; }
      std::this_thread::yield();
    }
    { std::lock_guard lock(release_mutex);
      if (released_escape_threads.size() != 2 || std::any_of(released_escape_threads.begin(),
          released_escape_threads.end(), [](auto thread) { return thread == std::this_thread::get_id(); }))
        return 1;
    }
    {
      std::lock_guard lock(statements_mutex);
      if (statements != std::vector<std::string>{"SELECT retained", "SELECT retained callback"})
        return 1;
    }
  }
  life.stop();
  if (!released || released_thread == std::this_thread::get_id()) return 1;
}
