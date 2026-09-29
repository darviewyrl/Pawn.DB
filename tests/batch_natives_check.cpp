#include <pawndb/connection_natives.hpp>

#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;

int main() {
  pawndb::Lifecycle life;
  life.start();
  AMX amx{};
  if (!life.attach(&amx)) return 1;
  struct Pool final : pawndb::SessionPool {
    bool query(std::string_view, pawndb::DriverError&) override { return true; }
    bool query_result(std::string_view sql, pawndb::DriverError& error,
                      pawndb::QueryResult& result) override {
      if (sql == "bad") { error = {1062, "duplicate key"}; return false; }
      result.fields = {"value"};
      result.rows = {{std::string("42")}};
      return true;
    }
  };
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
      [](std::string) {}, [](const auto&, auto&) { return std::make_shared<Pool>(); });
  std::unordered_map<cell, std::string> strings = {
      {100, "SELECT %d"}, {101, "bad"}, {102, "SELECT after"},
      {200, ""}, {201, ""}, {202, "OnBatchComplete"}, {203, ""}};
  std::unordered_map<cell, std::string> outputs;
  const auto* table = pawndb::ConnectionNatives::table();
  auto native = [table](const char* name) {
    for (auto* item = table; item->name; ++item)
      if (!std::strcmp(item->name, name)) return item->func;
    return static_cast<AMX_NATIVE>(nullptr);
  };
  auto call = [&](const char* name, std::initializer_list<cell> args) -> cell {
    const auto fn = native(name);
    if (!fn) return -999;
    std::vector<cell> params{static_cast<cell>(args.size() * sizeof(cell))};
    params.insert(params.end(), args.begin(), args.end());
    return fn(&amx, params.data());
  };
  cell callback_connection = 0, callback_result = 0;
  bool callback_ok = false, retain_in_callback = true;
  pawndb::ConnectionNatives natives(manager,
      [&](AMX*, cell address, std::string& value) {
        const auto it = strings.find(address);
        if (it == strings.end()) return false;
        value = it->second;
        return true;
      },
      [&](AMX*, cell address, std::string_view value, std::size_t capacity) {
        if (!capacity) return false;
        outputs[address] = value.substr(0, capacity - 1);
        return true;
      }, [] { return false; },
      [&](AMX*, cell address, std::size_t& length) {
        const auto it = strings.find(address);
        if (it == strings.end()) return false;
        length = it->second.size();
        return true;
      },
      [&](AMX*, cell address, std::span<char> output) {
        const auto it = strings.find(address);
        if (it == strings.end() || output.empty() || it->second.size() >= output.size()) return false;
        std::memcpy(output.data(), it->second.data(), it->second.size());
        output[it->second.size()] = '\0';
        return true;
      },
      [&](AMX*, std::string_view name, const auto& args) {
        if (name != "OnBatchComplete" || args.size() != 2) return;
        callback_connection = std::get<cell>(args[0]);
        callback_result = std::get<cell>(args[1]);
        const auto size = call("pdb_batch_size", {callback_result});
        if (retain_in_callback) {
          const auto first_status = call("pdb_batch_status", {callback_result, 0});
          const auto error_status = call("pdb_batch_status", {callback_result, 1});
          const auto last_status = call("pdb_batch_status", {callback_result, 2});
          const auto child = call("pdb_batch_get_result", {callback_result, 0});
          const auto rows = call("pdb_num_rows", {child});
          const auto error = call("pdb_batch_get_error", {callback_result, 1, 300, 64});
          callback_ok = size == 3 && first_status == 0 && error_status == 1 &&
              last_status == 0 && child > 0 && rows == 1 && error &&
              outputs[300] == "duplicate key" &&
              call("pdb_retain_batch_result", {callback_result});
        } else {
          callback_ok = size == 1 && call("pdb_batch_status", {callback_result, 0}) == 0;
        }
      });

  pawndb::ConnectionConfig config;
  config.host = "localhost";
  config.user = "test";
  config.database = "test";
  const auto connection = manager.connect(&amx, config);
  const auto connect_deadline = std::chrono::steady_clock::now() + 3s;
  while (!manager.is_connected(connection) && std::chrono::steady_clock::now() < connect_deadline)
    std::this_thread::yield();
  if (!manager.is_connected(connection)) return 2;

  const auto batch = call("pdb_batch_create", {static_cast<cell>(connection), 0});
  if (batch <= 0 || !call("pdb_batch_add", {batch, 100, 7}) ||
      !call("pdb_batch_add", {batch, 101}) || !call("pdb_batch_add", {batch, 102})) return 3;
  if (!call("pdb_batch_execute", {batch, 200, 201})) return 4;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!callback_result && std::chrono::steady_clock::now() < deadline) {
    life.dispatch_tick();
    std::this_thread::yield();
  }
  if (!callback_result || callback_connection != static_cast<cell>(connection) || !callback_ok ||
      !manager.batch_result(&amx, static_cast<std::uint32_t>(callback_result))) return 5;
  if (!call("pdb_free_batch_result", {callback_result}) ||
      manager.batch_result(&amx, static_cast<std::uint32_t>(callback_result))) return 6;

  callback_result = 0;
  callback_ok = false;
  retain_in_callback = false;
  const auto scoped_batch = call("pdb_batch_create", {static_cast<cell>(connection), 0});
  if (scoped_batch <= 0 || !call("pdb_batch_add", {scoped_batch, 102}) ||
      !call("pdb_batch_execute", {scoped_batch, 200, 201})) return 7;
  const auto scoped_deadline = std::chrono::steady_clock::now() + 3s;
  while (!callback_result && std::chrono::steady_clock::now() < scoped_deadline) {
    life.dispatch_tick();
    std::this_thread::yield();
  }
  if (!callback_result || !callback_ok ||
      manager.batch_result(&amx, static_cast<std::uint32_t>(callback_result))) return 8;
  if (!manager.close(connection)) return 9;
  life.detach(&amx);
  life.stop();
}
