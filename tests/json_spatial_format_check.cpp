#include <pawndb/connection_natives.hpp>

#include <array>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr << "Format check failed at " << __LINE__ << '\n'; return 1; } } while (false)

namespace {
struct State {
  std::mutex mutex;
  std::condition_variable ready;
  bool blocked = false;
  std::vector<std::string> sql;
  std::thread::id main = std::this_thread::get_id();
  bool wrong_thread = false;
};
struct Pool final : pawndb::SessionPool {
  State& state;
  pawndb::Backend backend;
  Pool(State& s, pawndb::Backend b) : state(s), backend(b) {}
  bool query(std::string_view sql, pawndb::DriverError&) override {
    std::unique_lock lock(state.mutex);
    state.ready.wait(lock, [&] { return !state.blocked; });
    state.wrong_thread |= std::this_thread::get_id() == state.main;
    state.sql.emplace_back(sql);
    return true;
  }
  bool escape_string(std::string_view input, std::span<char> output, std::size_t& size) const override {
    size = 0;
    for (const char ch : input) {
      if (size + 2 >= output.size()) return false;
      if (ch == '\'' || (ch == '\\' && backend == pawndb::Backend::mariadb))
        output[size++] = backend == pawndb::Backend::postgres ? '\'' : '\\';
      output[size++] = ch;
    }
    return true;
  }
};
cell connection_handle = 0;
cell AMX_NATIVE_CALL fixture_connection(AMX*, pawndb::NativeParams) { return connection_handle; }
cell AMX_NATIVE_CALL probe(AMX* amx, pawndb::NativeParams p) {
  if (!p[1]) amx_RaiseError(amx, AMX_ERR_ASSERT);
  return p[1];
}
cell AMX_NATIVE_CALL equal(AMX* amx, pawndb::NativeParams p) {
  cell *a = nullptr, *b = nullptr;
  if (amx_GetAddr(amx, p[1], &a) || amx_GetAddr(amx, p[2], &b)) return false;
  int size_a = 0, size_b = 0;
  amx_StrLen(a, &size_a); amx_StrLen(b, &size_b);
  std::string first(static_cast<std::size_t>(size_a) + 1, '\0');
  std::string second(static_cast<std::size_t>(size_b) + 1, '\0');
  amx_GetString(first.data(), a, 0, first.size()); amx_GetString(second.data(), b, 0, second.size());
  if (first != second) std::cerr << first.c_str() << " != " << second.c_str() << '\n';
  return first == second;
}
}

int main(int argc, char** argv) {
  using namespace pawndb;
  State state;
  auto mysql = std::make_shared<Pool>(state, Backend::mariadb);
  auto postgres = std::make_shared<Pool>(state, Backend::postgres);
  const EscapeSnapshot my{Backend::mariadb, mysql}, pg{Backend::postgres, postgres};
  const auto f = [](float value) { return std::bit_cast<cell>(value); };
  const std::array<cell, 4> vector{f(1500.5f), f(-1200.25f), f(15.125f), f(90)};
  auto read = [](std::int32_t address) -> std::optional<std::string_view> {
    if (address == 1) return "{\"text\":\"O'Reilly\"}";
    if (address == 2) return "{\"name\":\"Somchai\",\"money\":";
    if (address == 3) return "18446744073709551615";
    return std::nullopt;
  };
  for (auto* snapshot : {&my, &pg}) {
    for (const auto specifier : {"%j", "'%j'", "' %j '", "'\t%j\n'"}) {
      const std::array<cell, 1> raw{1}, bad{2}, big{3};
      auto result = format_sql(specifier, raw, read, snapshot);
      CHECK(result && result.sql.view() == (snapshot == &my
          ? "'{\"text\":\"O\\'Reilly\"}'" : "'{\"text\":\"O''Reilly\"}'"));
      result = format_sql(specifier, bad, read, snapshot);
      CHECK(result && result.sql.view() == "NULL" && result.invalid_json == 1);
      result = format_sql(specifier, big, read, snapshot);
      CHECK(result && result.sql.view() == "'18446744073709551615'");
    }
    for (const auto specifier : {"%v", "%v3", "'%v'", "' %v3 '"}) {
      const auto result = format_sql(specifier, vector, read, snapshot);
      CHECK(result && result.sql.view() == "'1500.5,-1200.25,15.125'");
    }
    for (const auto specifier : {"%v4", "'%v4'", "' %v4 '"}) {
      const auto result = format_sql(specifier, vector, read, snapshot);
      CHECK(result && result.sql.view() == "'1500.5,-1200.25,15.125,90'");
      CHECK(format_sql(specifier, std::span(vector).first(2), read, snapshot).error == SqlFormatError::stack_underflow);
    }
    for (const auto invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
      auto values = vector; values[2] = f(invalid);
      CHECK(format_sql("%v3", values, read, snapshot).error == SqlFormatError::invalid_number);
    }
    CHECK(format_sql("'prefix %j'", std::array<cell, 1>{1}, read, snapshot).error == SqlFormatError::malformed);
    CHECK(format_sql("/* %j */", std::array<cell, 1>{1}, read, snapshot).error == SqlFormatError::malformed);
  }
  CHECK(format_sql("$$%j$$", std::array<cell, 1>{1}, read, &pg).error == SqlFormatError::malformed);
  CHECK(format_sql("E'%j'", std::array<cell, 1>{1}, read, &pg).error == SqlFormatError::malformed);
  CHECK(sql_format_cells("%j %v %v3 %v4 %%") == 11);

  Lifecycle life;
  life.start();
  AMX amx{}, foreign{};
  AMX_HEADER header{}; header.magic = AMX_MAGIC;
  std::array<cell, 4096> memory{};
  amx.base = reinterpret_cast<unsigned char*>(&header);
  amx.data = reinterpret_cast<unsigned char*>(memory.data());
  amx.hea = 3000 * sizeof(cell); amx.stk = 3500 * sizeof(cell); amx.stp = sizeof(memory);
  CHECK(life.attach(&amx) && life.attach(&foreign));
  int warnings = 0, payload_reads = 0, callbacks = 0;
  ConnectionManager manager(life, [](void*, auto, int, auto) {}, [&](auto) { ++warnings; },
      [&](const ConnectionConfig& config, auto&) { return std::make_shared<Pool>(state, config.backend); });
  ConnectionNatives natives(manager,
      [](AMX* script, cell address, std::string& text) {
        cell* ptr = nullptr; int length = 0;
        if (amx_GetAddr(script, address, &ptr) || amx_StrLen(ptr, &length)) return false;
        text.resize(static_cast<std::size_t>(length) + 1);
        if (amx_GetString(text.data(), ptr, 0, text.size())) return false;
        text.resize(static_cast<std::size_t>(length)); return true;
      }, [](AMX* script, cell address, std::string_view text, std::size_t capacity) {
        cell* ptr = nullptr;
        if (amx_GetAddr(script, address, &ptr)) return false;
        const std::string truncated(text.substr(0, capacity - 1));
        return amx_SetString(ptr, truncated.c_str(), 0, 0, capacity) == AMX_ERR_NONE;
      }, [] { return false; }, [&](AMX* script, cell address, std::size_t& size) {
        if (script == &amx && address == 512) ++payload_reads;
        cell* ptr = nullptr; int length = 0;
        if (amx_GetAddr(script, address, &ptr) || amx_StrLen(ptr, &length)) return false;
        size = static_cast<std::size_t>(length); return true;
      }, [](AMX* script, cell address, std::span<char> output) {
        cell* ptr = nullptr;
        return !amx_GetAddr(script, address, &ptr) &&
               !amx_GetString(output.data(), ptr, 0, output.size());
      }, [&](AMX*, auto, const auto&) { ++callbacks; });
  ConnectionConfig config; config.host = "localhost"; config.user = "test"; config.database = "test";
  const auto connection = manager.connect(&amx, config);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!manager.is_connected(connection) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  CHECK(manager.is_connected(connection)); connection_handle = static_cast<cell>(connection);
  auto store = [&](std::size_t index, const std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) memory[index + i] = static_cast<unsigned char>(text[i]);
    memory[index + text.size()] = 0;
    return static_cast<cell>(index * sizeof(cell));
  };
  auto call = [&](const char* name, std::initializer_list<cell> args) {
    std::vector<cell> params{static_cast<cell>(args.size() * sizeof(cell))};
    params.insert(params.end(), args);
    for (auto* item = natives.table(); item->name; ++item)
      if (!std::strcmp(item->name, name)) return item->func(&amx, params.data());
    return cell{0};
  };
  const auto format = store(0, "SELECT ' %j ', '%v3'"), payload = cell{512};
  const auto output = cell{2000 * sizeof(cell)};
  const auto json = manager.create_json(&amx, true);
  CHECK(json & HandleRegistry::kAddressExcluded);
  manager.json(&amx, json)->value["n"] = 7;
  memory[128] = static_cast<cell>(json);
  for (std::size_t i = 0; i < 3; ++i) memory[129 + i] = vector[i];
  auto formatted = [&] { return call("pdb_format", {connection_handle, output, 256, format, payload, 516, 520, 524}); };
  CHECK(formatted() && formatted());
  std::string expected = "SELECT '{\"n\":7}', '1500.5,-1200.25,15.125'";
  std::array<char, 256> actual{};
  amx_GetString(actual.data(), memory.data() + 2000, 0, actual.size());
  CHECK(actual.data() == expected && payload_reads == 0 && manager.json(&amx, json));
  CHECK(!call("pdb_format", {connection_handle, output, 256, format, payload, 516}));
  CHECK(!call("pdb_format", {connection_handle, output, 256, format, payload, 516, 520, amx.stp}));
  CHECK(!call("pdb_format", {connection_handle, output, 256, format, payload, 516, 520, 12000}));
  memory[130] = f(std::numeric_limits<float>::infinity()); CHECK(!formatted()); memory[130] = vector[1];
  const auto callback = store(64, "OnRows"), empty = store(80, "");
  const auto batch = call("pdb_batch_create", {connection_handle, 0});
  { std::lock_guard lock(state.mutex); state.blocked = true; }
  CHECK(call("pdb_execute", {connection_handle, format, payload, 516, 520, 524}));
  CHECK(call("pdb_execute", {connection_handle, format, payload, 516, 520, 524}));
  CHECK(call("pdb_query", {connection_handle, format, callback, empty, payload, 516, 520, 524}));
  CHECK(call("pdb_batch_add", {batch, format, payload, 516, 520, 524}));
  CHECK(call("pdb_batch_execute", {batch, callback, empty}));
  manager.json(&amx, json)->value["n"] = 99;
  CHECK(manager.free_json(&amx, json));
  store(0, "SELECT '%j'");
  const auto json_only = [&] { return call("pdb_format", {connection_handle, output, 256, format, payload}); };
  const auto foreign_json = manager.create_json(&foreign, true);
  for (const auto invalid : {json, foreign_json, std::uint32_t{0}, std::uint32_t{0xdeadbeef}}) {
    memory[128] = static_cast<cell>(invalid);
    const int reads = payload_reads, warned = warnings;
    CHECK(json_only() && payload_reads == reads && warnings > warned);
    amx_GetString(actual.data(), memory.data() + 2000, 0, actual.size()); CHECK(std::string_view(actual.data()) == "SELECT NULL");
  }
  store(128, "{\"n\":5}"); CHECK(json_only() && payload_reads == 1);
  store(128, "{\"name\":\"Somchai\",\"money\":"); CHECK(json_only());
  memory[128] = 'x';
  CHECK(call("pdb_format", {connection_handle, output, 256, format, -4}));
  { std::lock_guard lock(state.mutex); state.blocked = false; } state.ready.notify_all();
  const auto completed = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (callbacks < 2 && std::chrono::steady_clock::now() < completed) { life.dispatch_tick(); std::this_thread::yield(); }
  CHECK(callbacks == 2);
  { std::lock_guard lock(state.mutex);
    CHECK(state.sql.size() == 4 && !state.wrong_thread);
    for (const auto& statement : state.sql) CHECK(statement == expected);
  }

  if (argc == 2) {
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate); CHECK(file);
    const auto file_size = static_cast<std::size_t>(file.tellg());
    std::vector<unsigned char> program(file_size); file.seekg(0);
    CHECK(file.read(reinterpret_cast<char*>(program.data()), static_cast<std::streamsize>(file_size)));
    const auto* hdr = reinterpret_cast<const AMX_HEADER*>(program.data()); CHECK(hdr->magic == AMX_MAGIC);
    program.resize(static_cast<std::size_t>(hdr->stp) + 4096);
    AMX script{}; CHECK(amx_Init(&script, program.data()) == AMX_ERR_NONE);
    CHECK(life.attach(&script));
    const AMX_NATIVE_INFO fixtures[] = {{"fixture_connection", fixture_connection}, {"probe", probe}, {"equal", equal}, {nullptr, nullptr}};
    amx_Register(&script, fixtures, -1);
    CHECK(amx_Register(&script, natives.table(), natives.native_count) == AMX_ERR_NONE);
    // Use a connection owned by the running AMX, not the synthetic test AMX.
    const auto vm_connection = manager.connect(&script, config);
    const auto vm_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!manager.is_connected(vm_connection) && std::chrono::steady_clock::now() < vm_deadline) std::this_thread::yield();
    CHECK(manager.is_connected(vm_connection)); connection_handle = static_cast<cell>(vm_connection);
    cell result = 0; const int error = amx_Exec(&script, &result, AMX_EXEC_MAIN);
    if (error) std::cerr << "VM error " << error << " in " << argv[1] << '\n';
    CHECK(error == AMX_ERR_NONE);
    manager.shutdown(); manager.detach(&script); life.detach(&script); amx_Cleanup(&script);
    std::cout << "Executed 32-bit-cell fixture: " << argv[1] << '\n';
  }
  manager.detach(&amx); manager.detach(&foreign); life.stop();
}
