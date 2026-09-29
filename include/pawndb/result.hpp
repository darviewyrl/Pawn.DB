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

struct QueryResult {
  std::vector<std::string> fields;
  std::vector<std::vector<std::optional<std::string>>> rows;
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
      : owner(script), data(std::move(value)) {
    for (std::size_t i = 0; i < data.fields.size(); ++i)
      field_indices.emplace(data.fields[i], i);
  }

  void* owner;
  QueryResult data;
  std::unordered_map<std::string, std::size_t, ResultFieldHash, std::equal_to<>> field_indices;
  std::int64_t cursor = -1;
  bool retained = false;
};

}  // namespace pawndb
