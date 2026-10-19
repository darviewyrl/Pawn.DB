#include <pawndb/connection_natives.hpp>

#include <array>
#include <iostream>
#include <limits>

#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr << "JSON check failed at " << __LINE__ << '\n'; return 1; } } while (false)

int main() {
  pawndb::Lifecycle life;
  life.start();
  std::array<cell, 1024> memory{};
  AMX amx{}, other{};
  amx.data = reinterpret_cast<unsigned char*>(memory.data());
  amx.hea = 512 * sizeof(cell);
  amx.stk = 768 * sizeof(cell);
  amx.stp = static_cast<cell>(memory.size() * sizeof(cell));
  CHECK(life.attach(&amx) && life.attach(&other));
  int warnings = 0;
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
                                    [&](std::string) { ++warnings; });
  auto store = [&](std::size_t index, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) memory[index + i] = static_cast<unsigned char>(text[i]);
    memory[index + text.size()] = 0;
    return static_cast<cell>(index * sizeof(cell));
  };
  std::function<void()> callback;
  pawndb::ConnectionNatives::CryptoCompletion completion;
  pawndb::ConnectionNatives natives(manager,
      [&](AMX*, cell address, std::string& output) {
        output.clear();
        for (auto i = static_cast<std::size_t>(address) / sizeof(cell); memory[i]; ++i)
          output.push_back(static_cast<char>(memory[i]));
        return true;
      }, [&](AMX*, cell address, std::string_view value, std::size_t capacity) {
        const auto index = static_cast<std::size_t>(address) / sizeof(cell);
        const auto size = std::min(value.size(), capacity - 1);
        for (std::size_t i = 0; i < size; ++i) memory[index + i] = static_cast<unsigned char>(value[i]);
        memory[index + size] = 0;
        return true;
      }, [] { return false; }, {}, {},
      [&](AMX*, auto, const auto&) { callback(); }, {},
      [&](AMX*, auto, auto, bool, auto done) { completion = std::move(done); return true; });
  auto call = [&](const char* name, std::initializer_list<cell> args) -> cell {
    std::vector<cell> params{static_cast<cell>(args.size() * sizeof(cell))};
    params.insert(params.end(), args);
    for (auto* item = natives.table(); item->name; ++item)
      if (!std::strcmp(item->name, name)) return item->func(&amx, params.data());
    return 0;
  };
  const auto key = store(0, "value"), text = store(16, "hello \"world\""), output = cell{128 * sizeof(cell)};
  const auto f = [](float number) { return std::bit_cast<cell>(number); };
  int count = 0;
  for (auto* item = natives.table(); item->name; ++item) {
    CHECK(std::string_view(item->name).starts_with("pdb_"));
    ++count;
  }
  CHECK(count == natives.native_count && count == 75);
  const auto handle = call("pdb_json_create", {0});
  CHECK(handle && !call("pdb_json_create", {}));
  CHECK(call("pdb_json_to_string", {handle, output, 64}));
  CHECK(call("pdb_json_set_int", {handle, key, -42}));
  CHECK(manager.json(&amx, static_cast<std::uint32_t>(handle))->value["value"] == -42);
  CHECK(call("pdb_json_set_float", {handle, key, f(1.25f)}));
  CHECK(manager.json(&amx, static_cast<std::uint32_t>(handle))->value["value"] == 1.25);
  CHECK(call("pdb_json_set_bool", {handle, key, 1}));
  CHECK(manager.json(&amx, static_cast<std::uint32_t>(handle))->value["value"] == true);
  CHECK(call("pdb_json_set_str", {handle, key, text}));
  CHECK(manager.json(&amx, static_cast<std::uint32_t>(handle))->value["value"] == "hello \"world\"");
  CHECK(call("pdb_json_set_vector", {handle, key, f(1), f(2), f(3)}));
  CHECK(manager.json(&amx, static_cast<std::uint32_t>(handle))->value["value"] == nlohmann::json::array({1, 2, 3}));
  CHECK(call("pdb_json_set_vector4", {handle, key, f(1), f(2), f(3), f(90)}));
  auto object = manager.json(&amx, static_cast<std::uint32_t>(handle));
  const auto before = object->value;
  CHECK(before["value"] == nlohmann::json::array({1, 2, 3, 90}));
  const auto nan = f(std::numeric_limits<float>::quiet_NaN());
  const auto inf = f(std::numeric_limits<float>::infinity());
  CHECK(!call("pdb_json_set_float", {handle, key, nan}));
  CHECK(!call("pdb_json_set_float", {handle, key, inf}));
  CHECK(!call("pdb_json_set_vector", {handle, key, f(1), inf, f(3)}));
  CHECK(!call("pdb_json_set_vector4", {handle, key, f(1), f(2), f(3), nan}));
  CHECK(object->value == before);
  CHECK(call("pdb_json_to_string", {handle, output, 128}));
  std::string serialized;
  for (std::size_t i = 128; memory[i]; ++i) serialized.push_back(static_cast<char>(memory[i]));
  CHECK(nlohmann::json::parse(serialized) == before);
  memory[132] = 1234;
  CHECK(!call("pdb_json_to_string", {handle, output, 4}) && !memory[131] && memory[132] == 1234);
  CHECK(!call("pdb_json_to_string", {handle, output, 1}) && !memory[128]);
  memory[128] = 1234;
  CHECK(!call("pdb_json_to_string", {handle, output, 0}) && memory[128] == 1234);
  CHECK(!call("pdb_json_to_string", {handle, output, -1}) && memory[128] == 1234);
  CHECK(!call("pdb_json_to_string", {handle, cell{511 * sizeof(cell)}, 2}));
  CHECK(!call("pdb_json_to_string", {handle, cell{700 * sizeof(cell)}, 2}));
  CHECK(!call("pdb_json_to_string", {handle, output + 1, 8}));
  CHECK(!call("pdb_json_set_int", {handle, cell{700 * sizeof(cell)}, 1}));
  memory[511] = 'x';
  CHECK(!call("pdb_json_set_int", {handle, cell{511 * sizeof(cell)}, 1}));
  CHECK(!call("pdb_json_set_int", {handle, key}));
  CHECK(!manager.retain_json(&other, static_cast<std::uint32_t>(handle)));
  CHECK(!manager.free_json(&other, static_cast<std::uint32_t>(handle)));
  CHECK(!call("pdb_json_free", {0}) && !call("pdb_json_retain", {0}));
  CHECK(!call("pdb_json_set_int", {0, key, 1}));
  const auto wrong_type = manager.setup_init(&amx);
  CHECK(!call("pdb_json_set_int", {static_cast<cell>(wrong_type), key, 1}));
  CHECK(manager.free_setup(wrong_type));
  CHECK(call("pdb_json_free", {handle}) && !call("pdb_json_free", {handle}));
  CHECK(!call("pdb_json_to_string", {handle, output, 8}) && !memory[128]);
  object.reset();
  life.dispatch_tick();
  CHECK(!life.active_worker_pool());
  CHECK(!call("pdb_json_retain", {handle}));

  auto pending = manager.create_json(&amx, false);
  std::weak_ptr<pawndb::JsonObject> weak = manager.json(&amx, pending);
  const auto retained = call("pdb_json_create", {1});
  const auto promoted = call("pdb_json_create", {0});
  CHECK(call("pdb_json_retain", {promoted}) && call("pdb_json_retain", {promoted}));
  life.dispatch_tick();
  CHECK(weak.expired());
  CHECK(manager.json(&amx, static_cast<std::uint32_t>(retained)) &&
        manager.json(&amx, static_cast<std::uint32_t>(promoted)));
  CHECK(call("pdb_json_free", {retained}) && call("pdb_json_free", {promoted}));

  std::weak_ptr<pawndb::JsonObject> callback_retained;
  cell persistent = 0;
  {
    pawndb::ConnectionManager::JsonScope scope(manager, &amx);
    persistent = call("pdb_json_create", {1});
    CHECK(call("pdb_json_set_int", {persistent, key, 17}));
    callback_retained = manager.json(&amx, static_cast<std::uint32_t>(persistent));
  }
  life.dispatch_tick();
  CHECK(!callback_retained.expired() && call("pdb_json_to_string", {persistent, output, 128}));
  CHECK(call("pdb_json_free", {persistent}) && callback_retained.expired());

  std::uint32_t outer = 0, inner = 0;
  const auto host = manager.create_json(&amx, false);
  {
    pawndb::ConnectionManager::JsonScope scope(manager, &amx);
    outer = manager.create_json(&amx, false);
    {
      pawndb::ConnectionManager::JsonScope nested(manager, &amx);
      inner = manager.create_json(&amx, false);
    }
    CHECK(manager.json(&amx, outer) && !manager.json(&amx, inner));
    CHECK(manager.json(&amx, host));
  }
  CHECK(!manager.json(&amx, outer) && manager.json(&amx, host));
  life.dispatch_tick();
  CHECK(!manager.json(&amx, host));

  bool callback_ok = false;
  callback = [&] {
    outer = manager.create_json(&amx, false);
    pending = manager.create_json(&amx, false);
    callback_ok = manager.retain_json(&amx, pending);
    throw std::runtime_error("simulate callback failure");
  };
  const auto password = store(32, "pw"), callback_name = store(40, "OnHash");
  CHECK(call("pdb_hash", {password, callback_name}));
  completion("hash", false);
  CHECK(callback_ok && !manager.json(&amx, outer) && manager.json(&amx, pending));
  CHECK(manager.free_json(&amx, pending));

  std::vector<std::weak_ptr<pawndb::JsonObject>> stress;
  stress.reserve(10000);
  {
    pawndb::ConnectionManager::JsonScope scope(manager, &amx);
    for (int i = 0; i < 10000; ++i) {
      const auto item = call("pdb_json_create", {0});
      CHECK(item && call("pdb_json_set_int", {item, key, i}));
      CHECK(call("pdb_json_to_string", {item, output, 128}));
      stress.push_back(manager.json(&amx, static_cast<std::uint32_t>(item)));
    }
  }
  for (const auto& item : stress) CHECK(item.expired());
  std::cout << "10,000 scoped JSON objects destroyed\n";
  stress.clear();
  for (int i = 0; i < 10000; ++i) {
    const auto item = call("pdb_json_create", {0});
    CHECK(item && call("pdb_json_set_int", {item, key, i}));
    stress.push_back(manager.json(&amx, static_cast<std::uint32_t>(item)));
  }
  life.dispatch_tick();
  for (const auto& item : stress) CHECK(item.expired());
  std::cout << "10,000 pending JSON objects destroyed at ProcessTick\n";

  {
    pawndb::ConnectionManager::JsonScope scope(manager, &amx);
    outer = manager.create_json(&amx, false);
    inner = manager.create_json(&amx, true);
    manager.detach(&amx);
    life.detach(&amx);
    CHECK(!manager.json(&amx, outer) && !manager.json(&amx, inner));
    CHECK(!call("pdb_json_create", {0}));
    CHECK(life.attach(&amx));
  }
  pending = manager.create_json(&amx, false);
  weak = manager.json(&amx, pending);
  manager.detach(&amx);
  CHECK(weak.expired());
  life.dispatch_tick();
  CHECK(warnings >= 10);

  std::weak_ptr<pawndb::JsonObject> unloaded;
  {
    pawndb::Lifecycle plugin;
    plugin.start();
    CHECK(plugin.attach(&amx));
    pawndb::ConnectionManager owned(plugin, [](void*, auto, int, auto) {}, [](auto) {});
    unloaded = owned.json(&amx, owned.create_json(&amx, true));
  }
  CHECK(unloaded.expired());
  life.stop();
}
