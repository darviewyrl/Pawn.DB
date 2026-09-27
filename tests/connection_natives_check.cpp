#include <pawndb/connection_natives.hpp>

#include <string>

int main() {
  pawndb::Lifecycle life;
  life.start();
  AMX amx{};
  life.attach(&amx);
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
                                    [](std::string) {});
  std::string output;
  pawndb::ConnectionNatives natives(manager,
      [](AMX*, cell address, std::string& value) {
        switch (address) {
          case 1: value = "localhost"; return true;
          case 2: value = "root"; return true;
          case 3: value = ""; return true;
          case 4: value = "test"; return true;
          case 5: value = "utf8mb4"; return true;
          default: return false;
        }
      }, [&](AMX*, cell, std::string_view value, std::size_t capacity) {
        output = value.substr(0, capacity - 1);
        return true;
      });
  const auto* table = pawndb::ConnectionNatives::table();
  const cell connect[] = {7 * sizeof(cell), 1, 2, 3, 4, 5432, 5, 1};
  const cell handle = table[0].func(&amx, connect);
  if (!handle) return 1;
  cell one[] = {sizeof(cell), handle};
  if (table[3].func(&amx, one)) return 1;
  cell driver[] = {3 * sizeof(cell), handle, 9, 32};
  if (!table[7].func(&amx, driver) || output != "PostgreSQL") return 1;
  cell option[] = {3 * sizeof(cell), handle, 1, 8};
  if (!table[5].func(&amx, option)) return 1;
  if (!table[2].func(&amx, one) || table[2].func(&amx, one)) return 1;
  life.stop();
}
