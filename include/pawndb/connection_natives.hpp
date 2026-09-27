#pragma once

#include <pawndb/connection_manager.hpp>
#include <pawndb/sql_formatter.hpp>
#include <amx/amx.h>

#include <algorithm>
#include <functional>
#include <span>
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
  using StringLength = std::function<bool(AMX*, cell, std::size_t&)>;
  using StringCopy = std::function<bool(AMX*, cell, std::span<char>)>;
  using Write = std::function<bool(AMX*, cell, std::string_view, std::size_t)>;
  using UpdateAvailable = std::function<bool()>;

  ConnectionNatives(ConnectionManager& manager, Read read, Write write,
                    UpdateAvailable update_available, StringLength string_length = {},
                    StringCopy string_copy = {})
      : manager_(manager), read_(std::move(read)), write_(std::move(write)),
        update_available_(std::move(update_available)),
        string_length_(std::move(string_length)), string_copy_(std::move(string_copy)) {
    current_ = this;
  }
  ~ConnectionNatives() { current_ = nullptr; }

  static const AMX_NATIVE_INFO* table() {
    static const AMX_NATIVE_INFO natives[] = {
        {"pdb_connect", connect}, {"pdb_connect_file", connect_file},
        {"pdb_close", close}, {"pdb_is_connected", is_connected},
        {"pdb_set_debug_level", set_debug_level}, {"pdb_get_driver_name", get_driver_name},
        {"pdb_setup_init", setup_init}, {"pdb_setup_free", setup_free},
        {"pdb_setup_charset", setup_charset}, {"pdb_setup_option", setup_option},
        {"pdb_setup_driver", setup_driver}, {"pdb_setup_ssl", setup_ssl},
        {"pdb_is_update_available", is_update_available},
        {"pdb_format", format},
        {nullptr, nullptr}};
    return natives;
  }

  SqlFormatResult format_variadic(AMX* amx, NativeParams params) const {
    return format_at(amx, params, 1, 4, 5);
  }

 private:
  SqlFormatResult format_at(AMX* amx, NativeParams params, int handle_index,
                            int format_index, int first_variadic) const {
    const int count = argc(params);
    SqlFormatResult failure;
    failure.error = SqlFormatError::malformed;
    if (!amx || count < first_variadic - 1 || !string_length_ || !string_copy_) {
      manager_.report_format_error("[Pawn.DB Warning] Invalid SQL format arguments.");
      return failure;
    }
    try {
      PawnStringScratch format;
      const auto length = [this, amx](std::int32_t address, std::size_t& size) {
        return string_length_(amx, static_cast<cell>(address), size);
      };
      const auto copy = [this, amx](std::int32_t address, std::span<char> output) {
        return string_copy_(amx, static_cast<cell>(address), output);
      };
      if (!format.load(params[format_index], length, copy)) {
        failure.error = SqlFormatError::string_read;
        manager_.report_format_error("[Pawn.DB Warning] Unable to read SQL format string.");
        return failure;
      }
      const auto available = static_cast<std::size_t>(count - first_variadic + 1);
      const auto* args = reinterpret_cast<const std::int32_t*>(params + first_variadic);
      const auto snapshot = manager_.escape_snapshot(static_cast<std::uint32_t>(params[handle_index]));
      PawnStringScratch argument;
      auto result = format_sql(format.view(), std::span(args, available),
          [&](std::int32_t address) -> std::optional<std::string_view> {
            return argument.load(address, length, copy)
                       ? std::optional<std::string_view>(argument.view()) : std::nullopt;
          }, snapshot.get());
      if (result.error == SqlFormatError::stack_underflow) {
        manager_.report_format_error("[Pawn.DB Error] Stack parameter underflow in format string: expected " +
            std::to_string(result.expected) + " cells, got " + std::to_string(result.available) + " cells.");
      } else if (result.error == SqlFormatError::too_large) {
        manager_.report_format_error("[Pawn.DB Warning] SQL format exceeds the 65,536-byte limit.");
      } else if (result.error == SqlFormatError::escape_failed) {
        manager_.report_format_error("[Pawn.DB Warning] Driver SQL escaping failed.");
      } else if (result.error != SqlFormatError::none) {
        manager_.report_format_error("[Pawn.DB Warning] SQL formatting failed.");
      }
      return result;
    } catch (...) {
      failure.error = SqlFormatError::allocation_failure;
      manager_.report_format_error("[Pawn.DB Warning] SQL formatting ran out of memory.");
      return failure;
    }
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
      const auto setup = count >= 6 ? static_cast<std::uint32_t>(params[6]) : 0;
      const bool auto_free = count < 7 || params[7] != 0;
      return static_cast<cell>(current_->manager_.connect(amx, std::move(config), setup, auto_free));
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
  static cell AMX_NATIVE_CALL setup_init(AMX* amx, NativeParams params) {
    return current_ && argc(params) == 0
               ? static_cast<cell>(current_->manager_.setup_init(amx)) : 0;
  }
  static cell AMX_NATIVE_CALL setup_free(AMX*, NativeParams params) {
    return current_ && argc(params) == 1 && current_->manager_.free_setup(
        static_cast<std::uint32_t>(params[1]));
  }
  static cell AMX_NATIVE_CALL setup_charset(AMX* amx, NativeParams params) {
    if (!current_ || argc(params) != 2) return 0;
    try {
      std::string value;
      return read(amx, params[2], value) &&
             current_->manager_.setup_charset(static_cast<std::uint32_t>(params[1]),
                                              std::move(value));
    } catch (...) { return 0; }
  }
  static cell AMX_NATIVE_CALL setup_option(AMX*, NativeParams params) {
    return current_ && argc(params) == 3 && current_->manager_.setup_option(
        static_cast<std::uint32_t>(params[1]), params[2], params[3]);
  }
  static cell AMX_NATIVE_CALL setup_driver(AMX*, NativeParams params) {
    return current_ && argc(params) == 2 && current_->manager_.setup_driver(
        static_cast<std::uint32_t>(params[1]), params[2]);
  }
  static cell AMX_NATIVE_CALL setup_ssl(AMX* amx, NativeParams params) {
    const int count = argc(params);
    if (!current_ || count < 2 || count > 5) return 0;
    try {
      std::string ca, cert, key;
      if (!read(amx, params[2], ca) ||
          (count >= 3 && !read(amx, params[3], cert)) ||
          (count >= 4 && !read(amx, params[4], key))) return 0;
      return current_->manager_.setup_ssl(static_cast<std::uint32_t>(params[1]),
          std::move(ca), std::move(cert), std::move(key), count < 5 || params[5] != 0);
    } catch (...) { return 0; }
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

  static cell AMX_NATIVE_CALL is_update_available(AMX*, NativeParams params) {
    return current_ && argc(params) == 0 && current_->update_available_();
  }

  static constexpr int native_count = 14;

  static cell AMX_NATIVE_CALL format(AMX* amx, NativeParams params) {
    if (!current_ || argc(params) < 4 || params[3] <= 0) return 0;
    try {
      auto result = current_->format_at(amx, params, 1, 4, 5);
      const auto capacity = static_cast<std::size_t>(params[3]);
      const std::string_view output = result ? result.sql.view() : std::string_view("");
      if (!current_->write_(amx, params[2], output, capacity)) return 0;
      return static_cast<cell>(result ? std::min(output.size(), capacity - 1) : 0);
    } catch (...) { return 0; }
  }

  ConnectionManager& manager_;
  Read read_;
  Write write_;
  UpdateAvailable update_available_;
  StringLength string_length_;
  StringCopy string_copy_;
  inline static ConnectionNatives* current_ = nullptr;
};

}  // namespace pawndb
