#pragma once

#include <pawndb/connection_natives.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>

inline bool check_spatial_db_roundtrip(pawndb::QueryResult result) {
  pawndb::Lifecycle life;
  life.start();
  AMX amx{};
  std::array<cell, 512> memory{};
  amx.data = reinterpret_cast<unsigned char*>(memory.data());
  amx.hea = 256 * sizeof(cell); amx.stk = 300 * sizeof(cell); amx.stp = sizeof(memory);
  if (!life.attach(&amx)) return false;
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {}, [](std::string) {});
  pawndb::ConnectionNatives natives(manager, {}, {}, [] { return false; });
  const auto* table = pawndb::ConnectionNatives::table();
  auto find = [table](const char* name) {
    for (auto* item = table; item->name; ++item)
      if (!std::strcmp(item->name, name)) return item->func;
    return static_cast<AMX_NATIVE>(nullptr);
  };
  const auto handle = manager.create_result(&amx, std::move(result));
  const cell next[] = {0};
  if (!manager.activate_result(&amx, handle)) return false;
  constexpr cell x = 80 * sizeof(cell), y = 81 * sizeof(cell), z = 82 * sizeof(cell), w = 83 * sizeof(cell);
  const cell fetch[] = {6 * sizeof(cell), 0, x, y, z, w, -1};
  const auto next_row = find("pdb_next_row");
  const auto get_vector = find("pdb_get_vector4_by_index");
  bool passed = handle && next_row && get_vector && next_row(&amx, next) && get_vector(&amx, fetch);
  const auto value = [&](cell address) { return std::bit_cast<float>(memory[static_cast<std::size_t>(address / sizeof(cell))]); };
  passed = passed && std::abs(value(x) - 1532.125f) < 1e-4f &&
      std::abs(value(y) + 1688.5432f) < 1e-4f && std::abs(value(z) - 13.5f) < 1e-4f &&
      std::abs(value(w) - 270.0f) < 1e-4f;
  manager.detach(&amx);
  life.detach(&amx);
  life.stop();
  return passed;
}
