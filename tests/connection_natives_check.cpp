#include <pawndb/connection_natives.hpp>

#include <cstring>
#include <string>

int main() {
  pawndb::Lifecycle life;
  life.start();
  AMX amx{};
  life.attach(&amx);
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
                                    [](std::string) {});
  std::string output;
  std::size_t output_capacity = 0;
  int argument_reads = 0;
  pawndb::ConnectionNatives natives(manager,
      [](AMX*, cell address, std::string& value) {
        switch (address) {
          case 1: value = "localhost"; return true;
          case 2: value = "root"; return true;
          case 3: value = ""; return true;
          case 4: value = "test"; return true;
          case 5: value = "utf8mb4"; return true;
          case 6: value = "ca.pem"; return true;
          case 12: value = "%s"; return true;
          case 13: value = std::string(1024, 'x'); return true;
          default: return false;
        }
      }, [&](AMX*, cell, std::string_view value, std::size_t capacity) {
        output_capacity = capacity;
        output = value.substr(0, capacity - 1);
        output.push_back('\0');
        return true;
      }, [] { return true; }, [&](AMX*, cell address, std::size_t& length) {
        if (address == 11) ++argument_reads;
        const std::string value = address == 10 ? "SELECT %d, %s" :
                                  address == 11 ? "hello" : address == 12 ? "%s" :
                                  address == 13 ? std::string(1024, 'x') : "";
        if (value.empty()) return false;
        length = value.size();
        return true;
      }, [&](AMX*, cell address, std::span<char> output) {
        if (address == 11) ++argument_reads;
        const std::string value = address == 10 ? "SELECT %d, %s" :
                                  address == 11 ? "hello" : address == 12 ? "%s" :
                                  address == 13 ? std::string(1024, 'x') : "";
        if (value.empty() || output.size() < value.size() + 1) return false;
        std::memcpy(output.data(), value.data(), value.size());
        output[value.size()] = '\0';
        return true;
      });
  const auto* table = pawndb::ConnectionNatives::table();
  auto native = [table](const char* name) {
    for (auto* item = table; item->name; ++item)
      if (std::string(item->name) == name) return item->func;
    return static_cast<AMX_NATIVE>(nullptr);
  };
  const cell init[] = {0};
  cell no_args[] = {0};
  if (!native("pdb_is_update_available")(&amx, no_args)) return 1;
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
  const cell variadic[] = {6 * sizeof(cell), handle, 0, 0, 10, 42, 11};
  auto formatted = natives.format_variadic(&amx, variadic);
  if (!formatted || formatted.sql.view() != "SELECT 42, hello" || argument_reads != 2) return 1;
  argument_reads = 0;
  const cell underflow[] = {5 * sizeof(cell), handle, 0, 0, 10, 42};
  const auto rejected = natives.format_variadic(&amx, underflow);
  if (rejected.error != pawndb::SqlFormatError::stack_underflow || argument_reads) return 1;
  cell one[] = {sizeof(cell), handle};
  if (native("pdb_is_connected")(&amx, one)) return 1;
  cell driver_name[] = {3 * sizeof(cell), handle, 9, 32};
  if (!native("pdb_get_driver_name")(&amx, driver_name) ||
      std::string(output.c_str()) != "PostgreSQL") return 1;
  const cell format_1024[] = {5 * sizeof(cell), handle, 88, 64, 12, 13};
  if (native("pdb_format")(&amx, format_1024) != 63 || output.size() != 64 ||
      output[63] != '\0' || output_capacity != 64) return 1;
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
