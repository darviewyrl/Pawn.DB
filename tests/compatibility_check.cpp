#include <pawndb/connection_natives.hpp>
#include <pawndb/crypto.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr << "Compatibility check failed at " << __LINE__ << '\n'; return 1; } } while (false)

namespace {
pawndb::QueryResult rows() {
  pawndb::QueryResult data;
  data.fields = {"id", "score", "name", "flag", "nil"};
  data.rows = {{"42", "1.25", "pawn", "true", std::nullopt}, {"43", "2.5", "next", "false", "set"}};
  data.metadata = {77, 2, 1500, 1};
  return data;
}
pawndb::ConnectionManager* fixture_manager = nullptr;
cell connection = 0;
int finished_callbacks = 0;
cell AMX_NATIVE_CALL fixture_connection(AMX*, pawndb::NativeParams) { return connection; }
cell AMX_NATIVE_CALL fixture_result(AMX* amx, pawndb::NativeParams) {
  const auto result = fixture_manager->create_result(amx, rows());
  fixture_manager->activate_result(amx, result);
  return static_cast<cell>(result);
}
cell AMX_NATIVE_CALL probe(AMX* amx, pawndb::NativeParams p) {
  static int count = 0;
  ++count;
  if (!p[1]) {
    std::cerr << "Pawn compatibility probe failed at " << p[2] << " (probe " << count << ")\n";
    amx_RaiseError(amx, AMX_ERR_ASSERT);
  }
  return p[1];
}
cell AMX_NATIVE_CALL fixture_nested(AMX* amx, pawndb::NativeParams) {
  auto data = rows(); data.rows[0][0] = "99";
  const auto handle = fixture_manager->create_result(amx, std::move(data));
  pawndb::ConnectionManager::ResultScope scope(*fixture_manager, amx, handle);
  int index = -1; cell value = 0;
  return handle && !amx_FindPublic(amx, "OnNested", &index) && !amx_Exec(amx, &value, index) && value;
}
cell AMX_NATIVE_CALL finished(AMX*, pawndb::NativeParams) { return ++finished_callbacks; }
bool read(AMX* amx, cell address, std::string& output) {
  cell* ptr = nullptr; int size = 0;
  if (amx_GetAddr(amx, address, &ptr) || amx_StrLen(ptr, &size)) return false;
  output.resize(static_cast<std::size_t>(size) + 1);
  if (amx_GetString(output.data(), ptr, 0, output.size())) return false;
  output.resize(static_cast<std::size_t>(size)); return true;
}
cell AMX_NATIVE_CALL equal(AMX* amx, pawndb::NativeParams p) {
  std::string a, b; return read(amx, p[1], a) && read(amx, p[2], b) && a == b;
}
}

int main(int argc, char** argv) {
  using namespace pawndb;
  Lifecycle life; life.start();
  int warnings = 0;
  struct Pool final : SessionPool {
    bool query(std::string_view, DriverError&) override { return true; }
    bool query_result(std::string_view sql, DriverError&, QueryResult& data) override {
      if (sql == "never") return false;
      data = rows(); return true;
    }
  };
  ConnectionManager manager(life, [](void*, auto, int, std::string) {},
      [&](std::string) { ++warnings; }, [](const auto&, auto&) { return std::make_shared<Pool>(); });
  fixture_manager = &manager;
  AMX amx{}, other{};
  CHECK(life.attach(&amx) && life.attach(&other));
  std::array<cell, 64> memory{};
  amx.data = reinterpret_cast<unsigned char*>(memory.data());
  amx.hea = 32 * sizeof(cell); amx.stk = 32 * sizeof(cell); amx.stp = sizeof(memory);
  int vm_error = 0;
  CryptoEngine crypto(life);
  ConnectionNatives natives(manager,
      [&](AMX* script, cell address, std::string& text) {
        if (script == &amx) { if (address != 100) return false; text = "id"; return true; }
        return read(script, address, text);
      }, [](AMX* script, cell address, std::string_view text, std::size_t capacity) {
        cell* ptr = nullptr;
        return !amx_GetAddr(script, address, &ptr) && !amx_SetString(ptr, std::string(text).c_str(), 0, 0, capacity);
      }, [] { return true; },
      [](AMX* script, cell address, std::size_t& length) {
        cell* ptr = nullptr; int size = 0;
        if (amx_GetAddr(script, address, &ptr) || amx_StrLen(ptr, &size)) return false;
        length = static_cast<std::size_t>(size); return true;
      }, [](AMX* script, cell address, std::span<char> output) {
        cell* ptr = nullptr;
        return !amx_GetAddr(script, address, &ptr) && !amx_GetString(output.data(), ptr, 0, output.size());
      },
      [&](AMX* script, std::string_view name, const auto& args) {
        int index = -1;
        if (amx_FindPublic(script, std::string(name).c_str(), &index)) { vm_error = AMX_ERR_NOTFOUND; return; }
        std::vector<cell> strings;
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
          if (const auto* value = std::get_if<cell>(&*it)) amx_Push(script, *value);
          else {
            cell addr = 0; cell* ptr = nullptr;
            const int error = amx_PushString(script, &addr, &ptr, std::get<std::string>(*it).c_str(), 0, 0);
            if (error) { vm_error = error; return; }
            strings.push_back(addr);
          }
        }
        cell result = 0;
        const int error = amx_Exec(script, &result, index);
        if (error) vm_error = error;
        for (const auto addr : strings) amx_Release(script, addr);
      }, {}, [&](AMX* script, std::string password, std::string encoded, bool verify,
                 ConnectionNatives::CryptoCompletion completion) {
        return crypto.submit(script, std::move(password), std::move(encoded), verify, std::move(completion));
      });
  auto call = [&](AMX* script, const char* name, std::initializer_list<cell> args) {
    std::vector<cell> params{static_cast<cell>(args.size() * sizeof(cell))}; params.insert(params.end(), args);
    for (auto* item = natives.table(); item->name; ++item)
      if (!std::strcmp(item->name, name)) return item->func(script, params.data());
    return cell{-999};
  };
  int count = 0;
  for (auto* item = natives.table(); item->name; ++item) { CHECK(std::string_view(item->name).starts_with("pdb_")); ++count; }
  CHECK(count == 75 && count == natives.native_count);
  const auto outer = manager.create_result(&amx, rows());
  const auto inner = manager.create_result(&amx, rows());
  const auto foreign = manager.create_result(&other, rows());
  CHECK(call(&amx, "pdb_retain_result", {}) == 0);
  {
    ConnectionManager::ResultScope scope(manager, &amx, outer);
    CHECK(call(&amx, "pdb_get_int", {100, -1}) == 42);
    CHECK(call(&amx, "pdb_next_row", {}) && call(&amx, "pdb_get_int", {100, -1}) == 43);
    CHECK(call(&amx, "pdb_get_int", {100, 0}) == 42);
    CHECK(!call(&amx, "pdb_get_int", {100, -2}));
    CHECK(!call(&amx, "pdb_get_int", {100, 2}));
    CHECK(!call(&amx, "pdb_activate_result", {static_cast<cell>(foreign)}));
    CHECK(manager.active_result(&amx) == outer && manager.active_result(&other) == 0);
    {
      ConnectionManager::ResultScope empty(manager, &amx, 0);
      CHECK(manager.active_result(&amx) == 0);
    }
    CHECK(manager.active_result(&amx) == outer);
    try {
      ConnectionManager::ResultScope nested(manager, &amx, inner);
      CHECK(manager.active_result(&amx) == inner);
      CHECK(call(&amx, "pdb_retain_result", {}) == static_cast<cell>(inner));
      throw 7;
    } catch (int) {}
    CHECK(manager.active_result(&amx) == outer && manager.result(&amx, inner));
    CHECK(call(&amx, "pdb_activate_result", {0}) && manager.active_result(&amx) == 0);
    CHECK(call(&amx, "pdb_activate_result", {static_cast<cell>(inner)}));
    CHECK(call(&amx, "pdb_free_result", {static_cast<cell>(inner)}));
    CHECK(manager.active_result(&amx) == 0 && !call(&amx, "pdb_activate_result", {static_cast<cell>(inner)}));
  }
  CHECK(manager.active_result(&amx) == 0 && !manager.result(&amx, outer));
  const auto fresh = manager.create_result(&amx, rows());
  {
    ConnectionManager::ResultScope scope(manager, &amx, fresh);
    int visited = 0;
    while (call(&amx, "pdb_next_row", {})) ++visited;
    CHECK(visited == 2);
  }
  // A freed outer scope must never be resurrected on nested scope exit.
  const auto deleted = manager.create_result(&amx, rows()), nested = manager.create_result(&amx, rows());
  {
    ConnectionManager::ResultScope scope(manager, &amx, deleted);
    { ConnectionManager::ResultScope child(manager, &amx, nested); CHECK(manager.free_result(&amx, deleted)); }
    CHECK(manager.active_result(&amx) == 0);
  }
  CHECK(warnings >= 4);
  if (argc == 2) {
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate); CHECK(file);
    const auto size = static_cast<std::size_t>(file.tellg());
    std::vector<unsigned char> program(size); file.seekg(0);
    CHECK(file.read(reinterpret_cast<char*>(program.data()), static_cast<std::streamsize>(size)));
    CHECK(reinterpret_cast<const AMX_HEADER*>(program.data())->magic == AMX_MAGIC);
    program.resize(static_cast<std::size_t>(reinterpret_cast<const AMX_HEADER*>(program.data())->stp) + 4096);
    AMX script{}; CHECK(amx_Init(&script, program.data()) == AMX_ERR_NONE && life.attach(&script));
    const AMX_NATIVE_INFO fixtures[] = {{"fixture_connection", fixture_connection}, {"fixture_result", fixture_result},
        {"probe", probe}, {"equal", equal}, {"finished", finished}, {"fixture_nested", fixture_nested}, {nullptr, nullptr}};
    amx_Register(&script, fixtures, -1);
    CHECK(amx_Register(&script, natives.table(), natives.native_count) == AMX_ERR_NONE);
    ConnectionConfig config; config.host = "localhost"; config.user = "test"; config.database = "test";
    connection = static_cast<cell>(manager.connect(&script, config));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!manager.is_connected(static_cast<std::uint32_t>(connection)) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    CHECK(manager.is_connected(static_cast<std::uint32_t>(connection)));
    cell result = 0; CHECK(amx_Exec(&script, &result, AMX_EXEC_MAIN) == AMX_ERR_NONE);
    while (!vm_error && finished_callbacks < 8 && std::chrono::steady_clock::now() < deadline) {
      life.dispatch_tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(vm_error == 0 && finished_callbacks == 8);
    manager.shutdown(); manager.detach(&script); life.detach(&script); amx_Cleanup(&script);
    std::cout << "Executed compatibility bytecode (cell32): " << argv[1] << '\n';
  }
  manager.detach(&amx); manager.detach(&other); life.detach(&amx); life.detach(&other); life.stop();
}
