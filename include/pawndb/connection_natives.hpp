#pragma once

#include <pawndb/connection_manager.hpp>
#include <amx/amx.h>

#include <functional>
#include <string>
#include <string_view>

namespace pawndb {

#ifdef PAWNDB_OMP
using NativeParams = const cell*;
#else
using NativeParams = cell*;
#endif

class ConnectionNatives {
 public:
  using Read = std::function<bool(AMX*, cell, std::string&)>;
  using Write = std::function<bool(AMX*, cell, std::string_view, std::size_t)>;

  ConnectionNatives(ConnectionManager& manager, Read read, Write write)
      : manager_(manager), read_(std::move(read)), write_(std::move(write)) {
    current_ = this;
  }
  ~ConnectionNatives() { current_ = nullptr; }

  static const AMX_NATIVE_INFO* table() {
    static const AMX_NATIVE_INFO natives[] = {
        {"pdb_connect", connect}, {"pdb_connect_file", connect_file},
        {"pdb_close", close}, {"pdb_is_connected", is_connected},
        {"pdb_set_option", set_option}, {"pdb_set_option_int", set_option_int},
        {"pdb_set_debug_level", set_debug_level}, {"pdb_get_driver_name", get_driver_name},
        {nullptr, nullptr}};
    return natives;
  }

 private:
  static int argc(NativeParams params) {
    return params && params[0] >= 0 && params[0] % sizeof(cell) == 0
               ? params[0] / sizeof(cell) : -1;
  }
  static bool read(AMX* amx, cell address, std::string& output) {
    return current_ && current_->read_(amx, address, output);
  }

  static cell AMX_NATIVE_CALL connect(AMX* amx, NativeParams params) {
    const int count = argc(params);
    if (count < 4 || count > 7) return 0;
    try {
      ConnectionConfig config;
      if (!read(amx, params[1], config.host) || !read(amx, params[2], config.user) ||
          !read(amx, params[3], config.password) || !read(amx, params[4], config.database))
        return 0;
      if (count >= 5) config.port = params[5];
      if (count >= 6 && !read(amx, params[6], config.charset)) return 0;
      if (count >= 7) config.auto_reconnect = params[7] != 0;
      config.backend = config.port == 5432 ? Backend::postgres : Backend::mariadb;
      return static_cast<cell>(current_->manager_.connect(amx, std::move(config)));
    } catch (...) { return 0; }
  }

  static cell AMX_NATIVE_CALL connect_file(AMX* amx, NativeParams params) {
    if (argc(params) != 1) return 0;
    try {
      std::string path;
      return read(amx, params[1], path)
                 ? static_cast<cell>(current_->manager_.connect_file(amx, std::move(path))) : 0;
    } catch (...) { return 0; }
  }

  static cell AMX_NATIVE_CALL close(AMX*, NativeParams params) {
    return current_ && argc(params) == 1 &&
           current_->manager_.close(static_cast<std::uint32_t>(params[1]));
  }
  static cell AMX_NATIVE_CALL is_connected(AMX*, NativeParams params) {
    return current_ && argc(params) == 1 &&
           current_->manager_.is_connected(static_cast<std::uint32_t>(params[1]));
  }
  static cell AMX_NATIVE_CALL set_option(AMX* amx, NativeParams params) {
    if (!current_ || argc(params) != 3) return 0;
    try {
      std::string value;
      return read(amx, params[3], value) &&
             current_->manager_.set_option(static_cast<std::uint32_t>(params[1]), params[2],
                                           std::move(value));
    } catch (...) { return 0; }
  }
  static cell AMX_NATIVE_CALL set_option_int(AMX*, NativeParams params) {
    return current_ && argc(params) == 3 && current_->manager_.set_option_int(
               static_cast<std::uint32_t>(params[1]), params[2], params[3]);
  }
  static cell AMX_NATIVE_CALL set_debug_level(AMX*, NativeParams params) {
    return current_ && argc(params) == 2 && current_->manager_.set_debug_level(
               static_cast<std::uint32_t>(params[1]), params[2]);
  }
  static cell AMX_NATIVE_CALL get_driver_name(AMX* amx, NativeParams params) {
    if (!current_ || argc(params) != 3 || params[3] <= 0) return 0;
    auto name = current_->manager_.driver_name(static_cast<std::uint32_t>(params[1]));
    if (!name) {
      current_->write_(amx, params[2], "", static_cast<std::size_t>(params[3]));
      return 0;
    }
    return current_->write_(amx, params[2], *name, static_cast<std::size_t>(params[3]));
  }

  ConnectionManager& manager_;
  Read read_;
  Write write_;
  inline static ConnectionNatives* current_ = nullptr;
};

}  // namespace pawndb
