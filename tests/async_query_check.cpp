#include <pawndb/connection_natives.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
  pawndb::Lifecycle life;
  life.start();
  AMX amx{};
  life.attach(&amx);
  std::mutex mutex;
  std::condition_variable ready;
  bool block = false, entered = false;
  std::vector<std::string> statements;
  struct Pool final : pawndb::SessionPool {
    std::mutex& mutex;
    std::condition_variable& ready;
    bool& block;
    bool& entered;
    std::vector<std::string>& statements;
    Pool(std::mutex& m, std::condition_variable& cv, bool& b, bool& e,
         std::vector<std::string>& sql) : mutex(m), ready(cv), block(b), entered(e), statements(sql) {}
    bool query(std::string_view sql, pawndb::DriverError& error) override {
      std::unique_lock lock(mutex);
      statements.emplace_back(sql);
      if (sql == "SLOW") {
        entered = true;
        ready.notify_all();
        ready.wait(lock, [&] { return !block; });
      }
      if (sql == "INVALID SQL") { error = {1064, "syntax error"}; return false; }
      return true;
    }
    bool query_result(std::string_view sql, pawndb::DriverError& error,
                      pawndb::QueryResult& result) override {
      if (!query(sql, error)) return false;
      result.fields = {"value"};
      result.rows = {{std::string("row")}};
      return true;
    }
    bool ping(pawndb::DriverError&) override { return true; }
  };
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
      [](std::string) {}, [&](const auto&, auto&) {
        return std::make_shared<Pool>(mutex, ready, block, entered, statements);
      });
  pawndb::ConnectionManager::Handle handle = manager.connect(&amx, [] {
    pawndb::ConnectionConfig c;
    c.host = "localhost"; c.user = "user"; c.database = "db";
    return c;
  }());
  const auto connect_deadline = std::chrono::steady_clock::now() + 3s;
  while (!manager.is_connected(handle) && std::chrono::steady_clock::now() < connect_deadline)
    std::this_thread::yield();
  if (!handle || !manager.is_connected(handle)) { std::cerr << "connect\n"; return 1; }

  std::vector<std::pair<std::string, std::vector<pawndb::ConnectionNatives::CallbackArg>>> callbacks;
  std::uint32_t retained_result = 0;
  const auto main_thread = std::this_thread::get_id();
  std::thread::id callback_thread;
  const auto* table = pawndb::ConnectionNatives::table();
  const auto native = [table](const char* name) {
    for (auto* item = table; item->name; ++item)
      if (std::string(item->name) == name) return item->func;
    return static_cast<AMX_NATIVE>(nullptr);
  };
  const auto text_at = [](cell address) -> std::string {
    switch (address) {
      case 10: return "SELECT %d";
      case 11: return "OnLoaded";
      case 12: return "d";
      case 13: return "INVALID SQL";
      case 14: return "";
      case 15: return "SLOW";
      case 16: return "SELECT normal";
      case 17: return "SELECT high";
      case 18: return "OnSlow";
      default: return {};
    }
  };
  pawndb::ConnectionNatives natives(manager,
      [text_at](AMX*, cell address, std::string& output) {
        output = text_at(address); return address >= 10 && address <= 18;
      }, [](AMX*, cell, std::string_view, std::size_t) { return true; }, [] { return false; },
      [text_at](AMX*, cell address, std::size_t& length) {
        if (address < 10 || address > 18) return false;
        length = text_at(address).size(); return true;
      }, [text_at](AMX*, cell address, std::span<char> output) {
        if (address < 10 || address > 18 || output.size() <= text_at(address).size()) return false;
        const auto text = text_at(address);
        std::copy(text.begin(), text.end(), output.begin());
        output[text.size()] = '\0'; return true;
      }, [&](AMX*, std::string_view name, const auto& args) {
        callback_thread = std::this_thread::get_id();
        callbacks.emplace_back(name, args);
        if (name == "OnLoaded" && args.size() == 2) {
          retained_result = static_cast<std::uint32_t>(std::get<cell>(args[0]));
          const cell retain[] = {sizeof(cell), static_cast<cell>(retained_result)};
          if (!native("pdb_retain_result")(&amx, retain)) retained_result = 0;
        }
      });

  {
    std::lock_guard lock(mutex); block = true;
  }
  const cell slow[] = {4 * sizeof(cell), static_cast<cell>(handle), 15, 18, 14};
  const auto start = std::chrono::steady_clock::now();
  if (!native("pdb_query")(&amx, slow) ||
      std::chrono::steady_clock::now() - start > 50ms) { std::cerr << "slow enqueue\n"; return 1; }
  {
    std::unique_lock lock(mutex);
    if (!ready.wait_for(lock, 2s, [&] { return entered; })) { std::cerr << "slow not entered\n"; return 1; }
  }
  const cell normal[] = {2 * sizeof(cell), static_cast<cell>(handle), 16};
  const cell high[] = {5 * sizeof(cell), static_cast<cell>(handle), 17, 11, 12, 42};
  const auto enqueue_start = std::chrono::steady_clock::now();
  if (!native("pdb_execute")(&amx, normal) || !native("pdb_query")(&amx, high) ||
      std::chrono::steady_clock::now() - enqueue_start > 50ms || !callbacks.empty()) { std::cerr << "priority enqueue\n"; return 1; }
  for (int tick = 0; tick < 30; ++tick) {
    const auto tick_start = std::chrono::steady_clock::now();
    life.dispatch_tick();
    if (std::chrono::steady_clock::now() - tick_start > 10ms) { std::cerr << "blocked tick\n"; return 1; }
    std::this_thread::sleep_for(100ms);
  }
  {
    std::lock_guard lock(mutex); block = false;
  }
  ready.notify_all();
  const auto work_deadline = std::chrono::steady_clock::now() + 3s;
  while ((callbacks.size() < 2 || [&] { std::lock_guard lock(mutex); return statements.size() < 3; }()) &&
         std::chrono::steady_clock::now() < work_deadline) {
    life.dispatch_tick();
    std::this_thread::yield();
  }
  life.dispatch_tick();
  if (callbacks.size() != 2 || callbacks[1].first != "OnLoaded" ||
      callbacks[1].second.size() != 2 || !retained_result ||
      std::get<cell>(callbacks[1].second[0]) != static_cast<cell>(retained_result) ||
      std::get<cell>(callbacks[1].second[1]) != 42 ||
      callback_thread != main_thread) {
    std::cerr << "success callback: count=" << callbacks.size()
              << " name=" << (callbacks.size() < 2 ? "" : callbacks[1].first)
              << " args=" << (callbacks.size() < 2 ? 0 : callbacks[1].second.size())
              << " thread=" << (callback_thread == main_thread) << '\n'; return 1;
  }
  const auto stored = manager.result(&amx, retained_result);
  if (!stored || stored->cursor != -1 || stored->data.fields != std::vector<std::string>{"value"} ||
      stored->data.rows.size() != 1 || stored->data.rows[0][0] != "row") return 1;
  const cell free_result[] = {sizeof(cell), static_cast<cell>(retained_result)};
  if (!native("pdb_free_result")(&amx, free_result) ||
      manager.result(&amx, retained_result)) return 1;
  {
    std::lock_guard lock(mutex);
    if (statements != std::vector<std::string>{"SLOW", "SELECT high", "SELECT normal"}) { std::cerr << "priority order\n"; return 1; }
  }

  const cell invalid[] = {4 * sizeof(cell), static_cast<cell>(handle), 13, 11, 14};
  if (!native("pdb_query")(&amx, invalid)) { std::cerr << "invalid enqueue\n"; return 1; }
  const auto error_deadline = std::chrono::steady_clock::now() + 2s;
  while (callbacks.size() < 3 && std::chrono::steady_clock::now() < error_deadline) {
    life.dispatch_tick();
    std::this_thread::yield();
  }
  if (callbacks.size() != 3 || callbacks[2].first != "OnQueryError" ||
      std::get<cell>(callbacks[2].second[0]) != static_cast<cell>(handle) ||
      std::get<cell>(callbacks[2].second[1]) != 1064 ||
      std::get<std::string>(callbacks[2].second[2]) != "syntax error" ||
      std::get<std::string>(callbacks[2].second[3]) != "<redacted>") {
    std::cerr << "error callback: count=" << callbacks.size()
              << " name=" << (callbacks.size() < 3 ? "" : callbacks[2].first)
              << " args=" << (callbacks.size() < 3 ? 0 : callbacks[2].second.size()) << '\n';
    return 1;
  }

  {
    std::lock_guard lock(mutex); block = true; entered = false;
  }
  if (!native("pdb_query")(&amx, slow)) { std::cerr << "unload enqueue\n"; return 1; }
  {
    std::unique_lock lock(mutex);
    if (!ready.wait_for(lock, 2s, [&] { return entered; })) { std::cerr << "unload not entered\n"; return 1; }
  }
  manager.detach(&amx);
  life.detach(&amx);
  {
    std::lock_guard lock(mutex); block = false;
  }
  ready.notify_all();
  std::this_thread::sleep_for(50ms);
  life.dispatch_tick();
  if (callbacks.size() != 3) { std::cerr << "unload callback fired\n"; return 1; }
  life.stop();
}
