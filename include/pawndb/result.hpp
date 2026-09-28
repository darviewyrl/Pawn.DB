#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pawndb {

struct QueryResult {
  std::vector<std::string> fields;
  std::vector<std::vector<std::optional<std::string>>> rows;
};

struct ResultObject {
  explicit ResultObject(QueryResult value, void* script)
      : owner(script), data(std::move(value)) {}

  void* owner;
  QueryResult data;
  std::int64_t cursor = -1;
  bool retained = false;
};

}  // namespace pawndb
