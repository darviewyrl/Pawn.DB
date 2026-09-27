#pragma once

#include <pawndb/connection_config.hpp>

#include <functional>
#include <memory>
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
};

using SessionFactory = std::function<std::shared_ptr<SessionPool>(
    const ConnectionConfig&, DriverError&)>;

}  // namespace pawndb
