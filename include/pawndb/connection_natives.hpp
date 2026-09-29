#pragma once

#include <pawndb/connection_manager.hpp>
#include <pawndb/sql_formatter.hpp>
#include <amx/amx.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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
  using CellWrite = std::function<bool(AMX*, cell, cell)>;
  using CallbackArg = std::variant<cell, std::string>;
  using InvokeCallback = std::function<void(AMX*, std::string_view,
                                            const std::vector<CallbackArg>&)>;

  ConnectionNatives(ConnectionManager& manager, Read read, Write write,
                    UpdateAvailable update_available, StringLength string_length = {},
                    StringCopy string_copy = {}, InvokeCallback invoke_callback = {},
                    CellWrite cell_write = {})
      : manager_(manager), read_(std::move(read)), write_(std::move(write)),
        update_available_(std::move(update_available)),
        string_length_(std::move(string_length)), string_copy_(std::move(string_copy)),
        invoke_callback_(std::move(invoke_callback)), cell_write_(std::move(cell_write)) {
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
        {"pdb_get_stat", get_stat}, {"pdb_get_metrics", get_metrics},
        {"pdb_set_slow_query_threshold", set_slow_query_threshold},
        {"pdb_format", format}, {"pdb_execute", execute}, {"pdb_query", query},
        {"pdb_retain_result", retain_result}, {"pdb_free_result", free_result},
        {"pdb_num_rows", num_rows}, {"pdb_num_fields", num_fields},
        {"pdb_next_row", next_row}, {"pdb_seek_row", seek_row},
        {"pdb_field_name", field_name}, {"pdb_is_null", is_null},
        {"pdb_is_null_by_index", is_null_by_index}, {"pdb_get_int", get_int},
        {"pdb_get_float", get_float}, {"pdb_get_str", get_str},
        {"pdb_get_bool", get_bool}, {"pdb_get_int_by_index", get_int_by_index},
        {"pdb_get_float_by_index", get_float_by_index},
        {"pdb_get_str_by_index", get_str_by_index},
        {"pdb_get_bool_by_index", get_bool_by_index},
        {nullptr, nullptr}};
    return natives;
  }

  static constexpr int native_count = 36;

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

  bool submit(AMX* amx, NativeParams params, bool query_request) const {
    const int count = argc(params);
    constexpr int format_index = 2;
    const int first_variadic = query_request ? 5 : 3;
    if (!amx || count < first_variadic - 1) return false;
    std::string callback_name, specifiers;
    std::vector<CallbackArg> callback_args;
    if (query_request) {
      try {
        if (!read_(amx, params[3], callback_name) || callback_name.empty() ||
            !read_(amx, params[4], specifiers)) return false;
        const auto available = static_cast<std::size_t>(count - first_variadic + 1);
        if (specifiers.size() > available) return false;
        callback_args.reserve(specifiers.size());
        for (std::size_t i = 0; i < specifiers.size(); ++i) {
          const cell value = params[first_variadic + static_cast<int>(i)];
          if (specifiers[i] == 'd' || specifiers[i] == 'i' || specifiers[i] == 'f')
            callback_args.emplace_back(value);
          else if (specifiers[i] == 's') {
            std::string text;
            if (!read_(amx, value, text)) return false;
            callback_args.emplace_back(std::move(text));
          } else return false;
        }
      } catch (...) { return false; }
    }
    auto formatted = format_at(amx, params, 1, format_index, first_variadic);
    if (!formatted) return false;
    const auto handle = static_cast<std::uint32_t>(params[1]);
    auto invoke = invoke_callback_;
    auto* manager = &manager_;
    auto completion = [invoke = std::move(invoke), manager, amx, handle,
                       callback_name = std::move(callback_name),
                       callback_args = std::move(callback_args)]
        (bool ok, DriverError error, QueryResult result) {
      if (!invoke) return;
      if (ok && !callback_name.empty()) {
        const auto result_handle = manager->create_result(amx, std::move(result));
        if (!result_handle) {
          invoke(amx, "OnQueryError", {static_cast<cell>(handle), cell{-4},
                                        std::string("unable to allocate query result"),
                                        std::string("<redacted>")});
          return;
        }
        try {
          auto args = callback_args;
          args.insert(args.begin(), static_cast<cell>(result_handle));
          invoke(amx, callback_name, args);
        } catch (...) {}
        manager->release_scoped_result(amx, result_handle);
      } else if (!ok) {
        invoke(amx, "OnQueryError", {static_cast<cell>(handle), static_cast<cell>(error.code),
                                      std::move(error.message), std::string("<redacted>")});
      }
    };
    if (manager_.query(handle, std::string(formatted.sql.view()), std::move(completion),
                       query_request ? WorkerPool::Priority::high : WorkerPool::Priority::normal,
                       !query_request, query_request))
      return true;
    if (invoke_callback_)
      invoke_callback_(amx, "OnQueryError", {static_cast<cell>(handle), cell{-3},
                                               std::string("query was not accepted"),
                                               std::string("<redacted>")});
    return false;
  }

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

  static cell AMX_NATIVE_CALL get_stat(AMX*, NativeParams params) {
    if (!current_ || argc(params) != 2 || params[2] < 0 || params[2] > 3) return 0;
    const auto metrics = current_->manager_.metrics(static_cast<std::uint32_t>(params[1]));
    if (!metrics) return 0;
    const std::uint64_t values[] = {metrics->qps, metrics->pending,
                                    metrics->average_latency_us, metrics->slow_queries};
    return static_cast<cell>(std::min<std::uint64_t>(
        values[params[2]], static_cast<std::uint64_t>(std::numeric_limits<cell>::max())));
  }

  static cell AMX_NATIVE_CALL get_metrics(AMX* amx, NativeParams params) {
    if (!current_ || argc(params) != 5 || !current_->cell_write_) return 0;
    const auto metrics = current_->manager_.metrics(static_cast<std::uint32_t>(params[1]));
    if (!metrics) return 0;
    const std::uint64_t values[] = {metrics->qps, metrics->pending,
                                    metrics->average_latency_us, metrics->slow_queries};
    for (int i = 0; i < 4; ++i) {
      const auto value = std::min<std::uint64_t>(
          values[i], static_cast<std::uint64_t>(std::numeric_limits<cell>::max()));
      if (!current_->cell_write_(amx, params[i + 2], static_cast<cell>(value))) return 0;
    }
    return 1;
  }

  static cell AMX_NATIVE_CALL set_slow_query_threshold(AMX*, NativeParams params) {
    return current_ && argc(params) == 2 && params[2] >= 0 &&
           current_->manager_.set_slow_query_threshold(
               static_cast<std::uint32_t>(params[1]), static_cast<std::uint64_t>(params[2]));
  }

  static cell AMX_NATIVE_CALL format(AMX* amx, NativeParams params) {
    if (!current_ || argc(params) < 4 || params[3] <= 0) return 0;
    try {
      auto result = current_->format_at(amx, params, 1, 4, 5);
      const auto capacity = static_cast<std::size_t>(params[3]);
      const auto output = result ? result.sql.view() : std::string_view{};
      if (!current_->write_(amx, params[2], output, capacity)) return 0;
      return static_cast<cell>(result ? std::min(output.size(), capacity - 1) : 0);
    } catch (...) { return 0; }
  }

  static cell AMX_NATIVE_CALL execute(AMX* amx, NativeParams params) {
    return current_ && argc(params) >= 2 && current_->submit(amx, params, false);
  }

  static cell AMX_NATIVE_CALL query(AMX* amx, NativeParams params) {
    return current_ && argc(params) >= 4 && current_->submit(amx, params, true);
  }

  static cell AMX_NATIVE_CALL retain_result(AMX* amx, NativeParams params) {
    return current_ && argc(params) == 1 && current_->manager_.retain_result(
        amx, static_cast<std::uint32_t>(params[1]));
  }

  static cell AMX_NATIVE_CALL free_result(AMX* amx, NativeParams params) {
    return current_ && argc(params) == 1 && current_->manager_.free_result(
        amx, static_cast<std::uint32_t>(params[1]));
  }

  static std::shared_ptr<ResultObject> get_result(AMX* amx, NativeParams params) {
    return current_ && argc(params) >= 1
               ? current_->manager_.result(amx, static_cast<std::uint32_t>(params[1]))
               : nullptr;
  }

  static std::optional<std::size_t> column(ResultObject& result, cell index) {
    if (index < 0 || static_cast<std::size_t>(index) >= result.data.fields.size()) return {};
    return static_cast<std::size_t>(index);
  }

  static std::optional<std::size_t> column(ResultObject& result, std::string_view name) {
    const auto it = result.field_indices.find(name);
    return it == result.field_indices.end() ? std::nullopt : std::optional(it->second);
  }

  static bool read_field_name(AMX* amx, cell address, std::string& name) noexcept {
    try { return current_ && current_->read_(amx, address, name); }
    catch (...) { return false; }
  }

  static bool write_string(AMX* amx, cell address, std::string_view value,
                           std::size_t capacity) noexcept {
    try { return current_ && capacity && current_->write_(amx, address, value, capacity); }
    catch (...) { return false; }
  }

  static const std::optional<std::string>* value(ResultObject& result, std::size_t index) {
    if (result.cursor < 0 || static_cast<std::size_t>(result.cursor) >= result.data.rows.size())
      return nullptr;
    const auto& row = result.data.rows[static_cast<std::size_t>(result.cursor)];
    return index < row.size() ? &row[index] : nullptr;
  }

  static cell integer(const std::optional<std::string>* field) {
    if (!field || !*field) return 0;
    cell output{};
    const auto& text = **field;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? output : 0;
  }

  static cell real(const std::optional<std::string>* field) {
    static_assert(sizeof(cell) == sizeof(float));
    if (!field || !*field) return 0;
    float output{};
    const auto& text = **field;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output,
                                         std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                   std::isfinite(output)
               ? std::bit_cast<cell>(output) : 0;
  }

  static cell boolean(const std::optional<std::string>* field) {
    if (!field || !*field) return 0;
    const auto& text = **field;
    if (text == "1" || text == "t" || text == "T") return 1;
    if (text.size() != 4) return 0;
    constexpr std::string_view truth = "true";
    for (std::size_t i = 0; i < truth.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(text[i])) != truth[i]) return 0;
    return 1;
  }

  static cell string(AMX* amx, NativeParams params, int field_index, int output_index,
                     int capacity_index, bool by_name) {
    if (!current_ || argc(params) != capacity_index || params[capacity_index] <= 0) return 0;
    auto result = get_result(amx, params);
    std::optional<std::size_t> index;
    if (result) {
      if (by_name) {
        std::string name;
        if (read_field_name(amx, params[field_index], name)) index = column(*result, name);
      } else index = column(*result, params[field_index]);
    }
    const auto* field = result && index ? value(*result, *index) : nullptr;
    const bool valid = field && *field;
    const std::string_view text = valid ? std::string_view(**field) : std::string_view{};
    const bool written = write_string(amx, params[output_index], text,
                                      static_cast<std::size_t>(params[capacity_index]));
    return valid && written;
  }

  static cell num_rows(AMX* amx, NativeParams params) {
    if (argc(params) != 1) return 0;
    const auto result = get_result(amx, params);
    return result ? static_cast<cell>(std::min<std::size_t>(
                        result->data.rows.size(), std::numeric_limits<cell>::max())) : 0;
  }

  static cell num_fields(AMX* amx, NativeParams params) {
    if (argc(params) != 1) return 0;
    const auto result = get_result(amx, params);
    return result ? static_cast<cell>(std::min<std::size_t>(
                        result->data.fields.size(), std::numeric_limits<cell>::max())) : 0;
  }

  static cell next_row(AMX* amx, NativeParams params) {
    if (argc(params) != 1) return 0;
    const auto result = get_result(amx, params);
    if (!result || result->cursor + 1 >= static_cast<std::int64_t>(result->data.rows.size()))
      return 0;
    ++result->cursor;
    return 1;
  }

  static cell seek_row(AMX* amx, NativeParams params) {
    if (argc(params) != 2 || params[2] < 0) return 0;
    const auto result = get_result(amx, params);
    if (!result || static_cast<std::size_t>(params[2]) >= result->data.rows.size()) return 0;
    result->cursor = params[2];
    return 1;
  }

  static cell field_name(AMX* amx, NativeParams params) {
    if (!current_ || argc(params) != 4 || params[2] < 0 || params[4] <= 0) return 0;
    const auto result = get_result(amx, params);
    const auto index = result ? column(*result, params[2]) : std::nullopt;
    const bool written = write_string(amx, params[3],
        index ? std::string_view(result->data.fields[*index]) : std::string_view{},
        static_cast<std::size_t>(params[4]));
    return index.has_value() && written;
  }

  static cell is_null_at(AMX* amx, NativeParams params, int field_index, bool by_name) {
    auto result = get_result(amx, params);
    if (!result) return 0;
    std::optional<std::size_t> index;
    if (by_name) {
      std::string name;
      if (read_field_name(amx, params[field_index], name)) index = column(*result, name);
    } else index = column(*result, params[field_index]);
    if (!index) return 0;
    const auto* field = value(*result, *index);
    return field && !*field;
  }

  static cell is_null(AMX* amx, NativeParams params) {
    return current_ && argc(params) == 2 ? is_null_at(amx, params, 2, true) : 0;
  }

  static cell is_null_by_index(AMX* amx, NativeParams params) {
    return current_ && argc(params) == 2 ? is_null_at(amx, params, 2, false) : 0;
  }

  static cell get_number(AMX* amx, NativeParams params, bool by_name, bool floating,
                         bool boolean_value) {
    if (!current_ || argc(params) != 2) return 0;
    const auto result = get_result(amx, params);
    if (!result) return 0;
    std::optional<std::size_t> index;
    if (by_name) {
      std::string name;
      if (read_field_name(amx, params[2], name)) index = column(*result, name);
    } else index = column(*result, params[2]);
    const auto* field = index ? value(*result, *index) : nullptr;
    return boolean_value ? boolean(field) : floating ? real(field) : integer(field);
  }

  static cell get_int(AMX* amx, NativeParams params) {
    return get_number(amx, params, true, false, false);
  }
  static cell get_float(AMX* amx, NativeParams params) {
    return get_number(amx, params, true, true, false);
  }
  static cell get_bool(AMX* amx, NativeParams params) {
    return get_number(amx, params, true, false, true);
  }
  static cell get_int_by_index(AMX* amx, NativeParams params) {
    return get_number(amx, params, false, false, false);
  }
  static cell get_float_by_index(AMX* amx, NativeParams params) {
    return get_number(amx, params, false, true, false);
  }
  static cell get_bool_by_index(AMX* amx, NativeParams params) {
    return get_number(amx, params, false, false, true);
  }
  static cell get_str(AMX* amx, NativeParams params) {
    return string(amx, params, 2, 3, 4, true);
  }
  static cell get_str_by_index(AMX* amx, NativeParams params) {
    return string(amx, params, 2, 3, 4, false);
  }

  ConnectionManager& manager_;
  Read read_;
  Write write_;
  UpdateAvailable update_available_;
  StringLength string_length_;
  StringCopy string_copy_;
  InvokeCallback invoke_callback_;
  CellWrite cell_write_;
  inline static ConnectionNatives* current_ = nullptr;
};

}  // namespace pawndb
