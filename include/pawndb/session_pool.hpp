#pragma once

#include <pawndb/connection_config.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace pawndb {

struct DriverError {
  int code = -4;
  std::string message;
};

class SessionPool {
 public:
  virtual ~SessionPool() = default;
  virtual bool query(std::string_view sql, DriverError& error) = 0;
  virtual bool ping(DriverError& error) { return query("SELECT 1", error); }
  virtual bool escape_string(std::string_view, std::span<char>, std::size_t&) const {
    return false;
  }
};

struct EscapeSnapshot {
  Backend backend;
  std::shared_ptr<SessionPool> sessions;

  bool escape(std::string_view input, std::span<char> output, std::size_t& written) const {
    return sessions && sessions->escape_string(input, output, written);
  }
};

using SessionFactory = std::function<std::shared_ptr<SessionPool>(
    const ConnectionConfig&, DriverError&)>;

}  // namespace pawndb
