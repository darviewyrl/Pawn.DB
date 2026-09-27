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
          case 6: value = "ca.pem"; return true;
          default: return false;
        }
      }, [&](AMX*, cell, std::string_view value, std::size_t capacity) {
        output = value.substr(0, capacity - 1);
        return true;
      });
  const auto* table = pawndb::ConnectionNatives::table();
  auto native = [table](const char* name) {
    for (auto* item = table; item->name; ++item)
      if (std::string(item->name) == name) return item->func;
    return static_cast<AMX_NATIVE>(nullptr);
  };
  const cell init[] = {0};
  const cell setup = native("pdb_setup_init")(&amx, init);
  if (!setup) return 1;
  cell charset[] = {2 * sizeof(cell), setup, 5};
  cell driver[] = {2 * sizeof(cell), setup, 2};
  cell ssl[] = {2 * sizeof(cell), setup, 6};
  if (!native("pdb_setup_charset")(&amx, charset) ||
      !native("pdb_setup_driver")(&amx, driver) ||
      !native("pdb_setup_ssl")(&amx, ssl)) return 1;
  cell option[] = {3 * sizeof(cell), setup, 3, 2};
  cell invalid_option[] = {3 * sizeof(cell), setup, 4, 2};
  if (!native("pdb_setup_option")(&amx, option) ||
      native("pdb_setup_option")(&amx, invalid_option)) return 1;
  const cell connect[] = {6 * sizeof(cell), 1, 2, 3, 4, 3307, setup};
  const cell handle = native("pdb_connect")(&amx, connect);
  if (!handle) return 1;
  cell one[] = {sizeof(cell), handle};
  if (native("pdb_is_connected")(&amx, one)) return 1;
  cell driver_name[] = {3 * sizeof(cell), handle, 9, 32};
  if (!native("pdb_get_driver_name")(&amx, driver_name) || output != "PostgreSQL") return 1;
  cell free_setup[] = {sizeof(cell), setup};
  if (native("pdb_setup_charset")(&amx, charset) ||
      native("pdb_setup_free")(&amx, free_setup)) return 1;
  if (!native("pdb_close")(&amx, one) || native("pdb_close")(&amx, one)) return 1;
  const cell reusable = native("pdb_setup_init")(&amx, init);
  if (!reusable) return 1;
  cell reusable_charset[] = {2 * sizeof(cell), reusable, 5};
  cell connect_reuse[] = {7 * sizeof(cell), 1, 2, 3, 4, 5432, reusable, 0};
  const cell first = native("pdb_connect")(&amx, connect_reuse);
  const cell second = native("pdb_connect")(&amx, connect_reuse);
  cell free_reusable[] = {sizeof(cell), reusable};
  if (!first || !second || !native("pdb_setup_charset")(&amx, reusable_charset) ||
      !native("pdb_setup_free")(&amx, free_reusable) ||
      native("pdb_setup_free")(&amx, free_reusable)) return 1;
  life.stop();
}
