#include <pawndb/connection_natives.hpp>

#include <bit>
#include <cstring>
#include <string>
#include <unordered_map>

int main() {
  pawndb::Lifecycle life;
  life.start();
  AMX amx{};
  if (!life.attach(&amx)) return 1;
  int warnings = 0;
  pawndb::ConnectionManager manager(life, [](void*, auto, int, std::string) {},
                                    [&](std::string) { ++warnings; });
  const std::unordered_map<cell, std::string> names = {
      {101, "id"}, {102, "score"}, {103, "name"}, {104, "flag"},
      {105, "nullable"}, {106, "zero"}, {107, "empty"}, {108, "long"},
      {109, "missing"}};
  std::unordered_map<cell, std::string> outputs;
  pawndb::ConnectionNatives natives(manager,
      [&](AMX*, cell address, std::string& output) {
        const auto it = names.find(address);
        if (it == names.end()) return false;
        output = it->second;
        return true;
      },
      [&](AMX*, cell address, std::string_view value, std::size_t capacity) {
        if (!capacity) return false;
        outputs[address] = value.substr(0, capacity - 1);
        return true;
      }, [] { return false; });
  const auto* table = pawndb::ConnectionNatives::table();
  auto native = [table](const char* name) {
    for (auto* item = table; item->name; ++item)
      if (!std::strcmp(item->name, name)) return item->func;
    return static_cast<AMX_NATIVE>(nullptr);
  };

  pawndb::QueryResult data;
  data.fields = {"id", "score", "name", "flag", "nullable", "zero", "empty", "long"};
  data.metadata = {7, 2, 1234, 1};
  data.rows = {{"0", "1.25", "hello", "t", std::nullopt, "0", "", std::string(1024, 'x')},
               {"42", "-2.5", "next", "false", "set", std::nullopt, "", "tail"}};
  pawndb::QueryResultSet next_set{{"second"}, {{"next-set"}}};
  next_set.metadata = {9, 1, 987, 0};
  data.next_results.push_back(std::move(next_set));
  const auto handle = manager.create_result(&amx, std::move(data));
  if (!handle) return 1;
  const auto h = static_cast<cell>(handle);
  cell one[] = {sizeof(cell), h};
  cell integer_name[] = {2 * sizeof(cell), h, 101};
  cell integer_index[] = {2 * sizeof(cell), h, 0};
  cell string_name[] = {4 * sizeof(cell), h, 103, 201, 5};
  cell string_index[] = {4 * sizeof(cell), h, 7, 202, 5};
  cell empty_string[] = {4 * sizeof(cell), h, 6, 210, 4};
  cell null_string_index[] = {4 * sizeof(cell), h, 4, 209, 8};
  cell bool_name[] = {2 * sizeof(cell), h, 104};
  cell bool_index[] = {2 * sizeof(cell), h, 3};
  cell float_name[] = {2 * sizeof(cell), h, 102};
  cell float_index[] = {2 * sizeof(cell), h, 1};
  cell null_name[] = {2 * sizeof(cell), h, 105};
  cell null_index[] = {2 * sizeof(cell), h, 4};
  cell zero_name[] = {2 * sizeof(cell), h, 106};
  cell zero_index[] = {2 * sizeof(cell), h, 5};
  cell field_name[] = {4 * sizeof(cell), h, 1, 203, 5};

  if (pawndb::ConnectionNatives::native_count != 52 || !native("pdb_num_rows") ||
      !native("pdb_has_next_result") || !native("pdb_next_result") ||
      !native("pdb_insert_id") || !native("pdb_affected_rows") ||
      !native("pdb_exec_time") || !native("pdb_warning_count") ||
      native("pdb_insert_id")(&amx, one) != 7 || native("pdb_affected_rows")(&amx, one) != 2 ||
      native("pdb_exec_time")(&amx, one) != 1234 || native("pdb_warning_count")(&amx, one) != 1 ||
      native("pdb_num_rows")(&amx, one) != 2 || native("pdb_num_fields")(&amx, one) != 8 ||
      native("pdb_get_int")(&amx, integer_name) != 0 ||
      native("pdb_get_int_by_index")(&amx, integer_index) != 0 ||
      native("pdb_next_row")(&amx, one) != 1 ||
      native("pdb_get_int")(&amx, integer_name) != 0 ||
      native("pdb_get_int_by_index")(&amx, integer_index) != 0 ||
      !native("pdb_is_null")(&amx, null_name) || !native("pdb_is_null_by_index")(&amx, null_index) ||
      native("pdb_is_null")(&amx, zero_name) || native("pdb_is_null_by_index")(&amx, zero_index) ||
      native("pdb_get_int_by_index")(&amx, null_index) != 0 ||
      native("pdb_get_str_by_index")(&amx, null_string_index) || !outputs[209].empty() ||
      native("pdb_get_bool")(&amx, bool_name) != 1 ||
      native("pdb_get_bool_by_index")(&amx, bool_index) != 1 ||
      std::bit_cast<float>(native("pdb_get_float")(&amx, float_name)) != 1.25f ||
      std::bit_cast<float>(native("pdb_get_float_by_index")(&amx, float_index)) != 1.25f ||
      !native("pdb_get_str")(&amx, string_name) || outputs[201] != "hell" ||
      !native("pdb_get_str_by_index")(&amx, string_index) || outputs[202] != "xxxx" ||
      !native("pdb_get_str_by_index")(&amx, empty_string) || !outputs[210].empty() ||
      !native("pdb_field_name")(&amx, field_name) || outputs[203] != "scor") return 1;

  cell null_name_index[] = {2 * sizeof(cell), h, 105};
  cell null_index_index[] = {2 * sizeof(cell), h, 4};
  cell zero_name_index[] = {2 * sizeof(cell), h, 106};
  cell zero_index_index[] = {2 * sizeof(cell), h, 5};
  cell seek_row_one[] = {2 * sizeof(cell), h, 1};
  cell seek_row_invalid[] = {2 * sizeof(cell), h, 2};
  cell seek_row_negative[] = {2 * sizeof(cell), h, -1};
  cell nullable_string[] = {4 * sizeof(cell), h, 105, 208, 8};
  if (!native("pdb_seek_row")(&amx, seek_row_one) ||
      native("pdb_get_int_by_index")(&amx, integer_index) != 42 ||
      native("pdb_is_null")(&amx, null_name_index) ||
      native("pdb_is_null_by_index")(&amx, null_index_index) ||
      !native("pdb_is_null")(&amx, zero_name_index) ||
      !native("pdb_is_null_by_index")(&amx, zero_index_index) ||
      !native("pdb_get_str")(&amx, nullable_string) || outputs[208] != "set" ||
      native("pdb_get_int")(&amx, integer_name) != 42 ||
      native("pdb_get_bool_by_index")(&amx, bool_index) ||
      native("pdb_seek_row")(&amx, seek_row_negative) ||
      native("pdb_seek_row")(&amx, seek_row_invalid) ||
      native("pdb_get_int")(&amx, integer_name) != 42 || native("pdb_next_row")(&amx, one) ||
      !native("pdb_has_next_result")(&amx, one) || !native("pdb_next_result")(&amx, one) ||
      native("pdb_has_next_result")(&amx, one) || native("pdb_num_fields")(&amx, one) != 1 ||
      native("pdb_num_rows")(&amx, one) != 1 || native("pdb_insert_id")(&amx, one) != 9 ||
      native("pdb_affected_rows")(&amx, one) != 1 || native("pdb_exec_time")(&amx, one) != 987 ||
      native("pdb_warning_count")(&amx, one) != 0)
    return 1;
  cell next_field[] = {4 * sizeof(cell), h, 0, 203, 16};
  if (!native("pdb_field_name")(&amx, next_field) || outputs[203] != "second" ||
      !native("pdb_next_row")(&amx, one)) return 1;

  const auto empty = manager.create_result(&amx, {});
  cell empty_handle[] = {sizeof(cell), static_cast<cell>(empty)};
  cell empty_int[] = {2 * sizeof(cell), static_cast<cell>(empty), 0};
  cell empty_float[] = {2 * sizeof(cell), static_cast<cell>(empty), 0};
  cell empty_bool[] = {2 * sizeof(cell), static_cast<cell>(empty), 0};
  cell empty_str_name[] = {4 * sizeof(cell), static_cast<cell>(empty), 101, 205, 4};
  cell empty_str_index[] = {4 * sizeof(cell), static_cast<cell>(empty), 0, 206, 4};
  if (!empty || native("pdb_next_row")(&amx, empty_handle) ||
      native("pdb_num_rows")(&amx, empty_handle) ||
      native("pdb_insert_id")(&amx, empty_handle) ||
      native("pdb_affected_rows")(&amx, empty_handle) ||
      native("pdb_exec_time")(&amx, empty_handle) ||
      native("pdb_warning_count")(&amx, empty_handle) ||
      native("pdb_get_int")(&amx, empty_int) || native("pdb_get_float_by_index")(&amx, empty_float) ||
      native("pdb_get_bool_by_index")(&amx, empty_bool) ||
      native("pdb_get_str")(&amx, empty_str_name) || native("pdb_get_str_by_index")(&amx, empty_str_index) ||
      !outputs[205].empty() || !outputs[206].empty() ||
      !native("pdb_free_result")(&amx, empty_handle)) return 1;

  cell invalid_str[] = {4 * sizeof(cell), h, 109, 204, 8};
  cell invalid_index[] = {2 * sizeof(cell), h, 99};
  cell missing_bool[] = {2 * sizeof(cell), h, 109};
  cell bogus_handle[] = {sizeof(cell), 9999};
  cell bogus_str[] = {4 * sizeof(cell), 9999, 101, 207, 8};
  cell stale_float[] = {2 * sizeof(cell), h, 1};
  cell stale_bool[] = {2 * sizeof(cell), h, 0};
  if (native("pdb_get_int_by_index")(&amx, invalid_index) ||
      native("pdb_get_bool")(&amx, missing_bool) ||
      native("pdb_num_rows")(&amx, bogus_handle) || native("pdb_insert_id")(&amx, bogus_handle) ||
      native("pdb_affected_rows")(&amx, bogus_handle) || native("pdb_exec_time")(&amx, bogus_handle) ||
      native("pdb_warning_count")(&amx, bogus_handle) || native("pdb_get_str")(&amx, bogus_str) ||
      !outputs[207].empty() ||
      native("pdb_get_str")(&amx, invalid_str) || !outputs[204].empty() ||
      !native("pdb_free_result")(&amx, one) ||
      native("pdb_num_fields")(&amx, one) || native("pdb_get_int")(&amx, integer_name) ||
      native("pdb_insert_id")(&amx, one) || native("pdb_affected_rows")(&amx, one) ||
      native("pdb_exec_time")(&amx, one) || native("pdb_warning_count")(&amx, one) ||
      std::bit_cast<float>(native("pdb_get_float_by_index")(&amx, stale_float)) != 0.0f ||
      native("pdb_get_bool_by_index")(&amx, stale_bool) || native("pdb_get_str")(&amx, invalid_str) ||
      !outputs[204].empty() || warnings < 2) return 1;

  life.detach(&amx);
  life.stop();
}
