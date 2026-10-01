#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pawndb {

struct QueryResultSet {
  std::vector<std::string> fields;
  std::vector<std::vector<std::optional<std::string>>> rows;
  struct Metadata {
    std::uint64_t insert_id = 0;
    std::uint64_t affected_rows = 0;
    std::uint64_t exec_time_us = 0;
    std::uint64_t warning_count = 0;
  } metadata;
};

struct QueryResult : QueryResultSet {
  std::vector<QueryResultSet> next_results;
};

enum class BatchItemStatus : int { success, sql_error, not_executed, rolled_back };

struct BatchResultEntry {
  BatchItemStatus status = BatchItemStatus::not_executed;
  std::uint32_t result_handle = 0;
  int error_code = 0;
  std::string error_message;
};

struct BatchResultObject {
  explicit BatchResultObject(void* script) : owner(script) {}
  void* owner;
  std::vector<BatchResultEntry> items;
  bool retained = false;
};

struct ResultFieldHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
};

struct ResultObject {
  explicit ResultObject(QueryResult value, void* script)
      : owner(script), next_results(std::move(value.next_results)) {
    static_cast<QueryResultSet&>(data) = std::move(value);
    index_fields();
  }

  bool has_next_result() const noexcept { return next_result_index < next_results.size(); }
  bool next_result() {
    if (!has_next_result()) return false;
    data = {};
    static_cast<QueryResultSet&>(data) = std::move(next_results[next_result_index++]);
    cursor = -1;
    field_indices.clear();
    index_fields();
    return true;
  }

  void index_fields() {
    for (std::size_t i = 0; i < data.fields.size(); ++i)
      field_indices.emplace(data.fields[i], i);
  }

  void* owner;
  QueryResult data;
  std::vector<QueryResultSet> next_results;
  std::size_t next_result_index = 0;
  std::unordered_map<std::string, std::size_t, ResultFieldHash, std::equal_to<>> field_indices;
  std::int64_t cursor = -1;
  bool retained = false;
};

}  // namespace pawndb
