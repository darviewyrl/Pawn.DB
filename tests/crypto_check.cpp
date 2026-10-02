#include <pawndb/crypto.hpp>
#include <pawndb/connection_natives.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;

int main() {
  pawndb::Lifecycle life;
  life.start();
  pawndb::CryptoEngine crypto(life);
  AMX amx{};
  std::array<cell, 32> memory{};
  memory[1] = 77;
  amx.data = reinterpret_cast<unsigned char*>(memory.data());
  amx.hea = 64;
  amx.stk = 64;
  amx.stp = sizeof(memory);
  if (!life.attach(&amx)) return 1;
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
                                    [](std::string) {});
  std::unordered_map<cell, std::string> strings = {
      {10, "secret"}, {11, "wrong"}, {12, "HashDone"}, {13, "d"},
      {14, "VerifyDone"}, {15, ""}, {16, ""}, {17, "malformed"}};
  const auto main_thread = std::this_thread::get_id();
  int hashes = 0, verified = 0;
  bool passed = true;
  std::string first_hash, second_hash;
  pawndb::ConnectionNatives natives(manager,
      [&](AMX*, cell address, std::string& value) {
        const auto it = strings.find(address);
        if (it == strings.end()) return false;
        value = it->second;
        return true;
      }, [](AMX*, cell, std::string_view, std::size_t) { return true; },
      [] { return false; }, {}, {},
      [&](AMX*, std::string_view name, const auto& args) {
        passed &= std::this_thread::get_id() == main_thread;
        if (name == "HashDone") {
          passed &= args.size() == 2 && std::get<cell>(args[0]) == 77;
          const auto& hash = std::get<std::string>(args[1]);
          passed &= hash.starts_with("$argon2id$v=19$m=65536,t=3,p=4$");
          if (!hashes) first_hash = hash;
          if (hashes == 1) second_hash = hash;
          ++hashes;
        } else if (name == "VerifyDone") {
          passed &= args.size() == 1 && std::get<cell>(args[0]) == (verified == 0 ? 1 : 0);
          ++verified;
        } else passed = false;
      }, {}, [&](AMX* script, std::string password, std::string encoded, bool verify,
                 pawndb::ConnectionNatives::CryptoCompletion completion) {
        return crypto.submit(script, std::move(password), std::move(encoded), verify,
                             std::move(completion));
      });
  auto call = [&](const char* name, std::initializer_list<cell> args) {
    const auto* table = pawndb::ConnectionNatives::table();
    for (auto* item = table; item->name; ++item) {
      if (std::strcmp(item->name, name)) continue;
      std::vector<cell> params{static_cast<cell>(args.size() * sizeof(cell))};
      params.insert(params.end(), args);
      return item->func(&amx, params.data());
    }
    return cell{0};
  };
  if (call("pdb_hash", {10, 12, 13}) || call("pdb_hash", {10, 12, 13, 128})) return 1;
  for (int i = 0; i < 100; ++i)
    if (!call("pdb_hash", {10, 12, 13, 4})) return 1;
  strings[10] = "changed after enqueue";
  memory[1] = 99;
  const auto deadline = std::chrono::steady_clock::now() + 180s;
  auto wait = [&](auto done) {
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      const auto start = std::chrono::steady_clock::now();
      life.dispatch_tick();
      if (std::chrono::steady_clock::now() - start > 50ms) passed = false;
      std::this_thread::sleep_for(1ms);
    }
    return done();
  };
  if (!wait([&] { return hashes == 100; }) || !passed || first_hash == second_hash) return 1;
  strings[10] = "secret";
  strings[16] = first_hash;
  // Verify sequentially so callback order does not depend on worker scheduling.
  if (!call("pdb_verify", {10, 16, 14, 15}) ||
      !wait([&] { return verified == 1; }) ||
      !call("pdb_verify", {11, 16, 14, 15}) ||
      !wait([&] { return verified == 2; }) ||
      !call("pdb_verify", {10, 17, 14, 15}) ||
      !wait([&] { return verified == 3; })) return 1;
  if (pawndb::verify_password("secret", "$argon2id$v=19$m=4294967295,t=3,p=4$x$x")) return 1;
  AMX unloaded{};
  life.attach(&unloaded);
  int stale_callbacks = 0;
  if (!crypto.submit(&unloaded, "secret", first_hash, true,
      [&](std::string, bool) { ++stale_callbacks; })) return 1;
  life.detach(&unloaded);
  std::atomic<unsigned> barriers{0};
  auto* pool = life.worker_pool();
  for (std::size_t i = 0; i < pool->size(); ++i)
    if (!pool->submit(i, pawndb::WorkerPool::Priority::normal,
                     [&] { ++barriers; })) return 1;
  if (!wait([&] { return barriers.load() == pool->size(); })) return 1;
  life.dispatch_tick();
  life.stop();
  if (!passed || stale_callbacks) return 1;
  std::cout << "100 Argon2id hashes, correct/wrong/malformed verify, main-thread callbacks and unload guard passed\n";
}
