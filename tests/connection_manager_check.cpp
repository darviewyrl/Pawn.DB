#include <pawndb/connection_manager.hpp>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

int main() {
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
  auto handle = manager.connect(&script, direct);
  if (!handle || manager.is_connected(handle) || !manager.driver_name(handle) ||
      !manager.set_option(handle, 0, "latin1") || !manager.set_option_int(handle, 1, 8) ||
      !manager.set_debug_level(handle, 2)) return 1;
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
  {
    pawndb::ConnectionManager failed(life,
        [&](void* amx, auto, int code, std::string message) {
          if (amx == &script && code == 1045 && message == "access denied") ++driver_errors;
        }, [](std::string) {},
        [](const pawndb::ConnectionConfig&, pawndb::DriverError& error)
            -> std::shared_ptr<pawndb::SessionPool> {
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
  }
  life.stop();
  if (!released || released_thread == std::this_thread::get_id()) return 1;
}
