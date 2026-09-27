#pragma once

#include <pawndb/session_pool.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pawndb {

inline constexpr std::size_t kSqlInlineCapacity = 4096;
inline constexpr std::size_t kSqlMaxSize = 65536;

class SqlBuffer {
 public:
  bool append(std::string_view text) {
    if (text.size() > kSqlMaxSize - size_) return false;
    if (!spilled_ && size_ + text.size() <= inline_.size()) {
      std::copy(text.begin(), text.end(), inline_.begin() + size_);
    } else {
      if (!spilled_) {
        heap_.assign(inline_.data(), size_);
        spilled_ = true;
      }
      heap_.append(text);
    }
    size_ += text.size();
    return true;
  }

  std::string_view view() const {
    return spilled_ ? std::string_view(heap_) : std::string_view(inline_.data(), size_);
  }
  std::size_t size() const { return size_; }

 private:
  std::array<char, kSqlInlineCapacity> inline_{};
  std::string heap_;
  std::size_t size_ = 0;
  bool spilled_ = false;
};

enum class SqlFormatError { none, malformed, stack_underflow, string_read, escape_unavailable,
                            escape_failed, too_large, allocation_failure };

struct SqlFormatResult {
  SqlBuffer sql;
  SqlFormatError error = SqlFormatError::none;
  std::size_t expected = 0;
  std::size_t available = 0;
  explicit operator bool() const { return error == SqlFormatError::none; }
};

class PawnStringScratch {
 public:
  template <typename Length, typename Copy>
  bool load(std::int32_t address, const Length& length, const Copy& copy) {
    std::size_t size = 0;
    if (!length(address, size) || size > kSqlMaxSize) return false;
    std::span<char> target;
    if (size + 1 <= inline_.size()) {
      target = inline_;
    } else {
      heap_.resize(size + 1);
      target = heap_;
    }
    if (!copy(address, target)) return false;
    view_ = {target.data(), size};
    return true;
  }
  std::string_view view() const { return view_; }

 private:
  std::array<char, kSqlInlineCapacity> inline_{};
  std::string heap_;
  std::string_view view_;
};

inline std::optional<std::size_t> sql_format_cells(std::string_view format) {
  std::size_t cells = 0;
  for (std::size_t i = 0; i < format.size(); ++i) {
    if (format[i] != '%') continue;
    if (++i == format.size()) return std::nullopt;
    if (format[i] == '%') continue;
    if (format[i] != 'd' && format[i] != 'i' && format[i] != 'f' &&
        format[i] != 's' && format[i] != 'e' && format[i] != 'I' && format[i] != 'b')
      return std::nullopt;
    ++cells;
  }
  return cells;
}

inline bool append_number(SqlBuffer& output, std::int32_t value, bool floating) {
  std::array<char, 64> text{};
  const auto result = floating
      ? std::to_chars(text.data(), text.data() + text.size(),
                      std::bit_cast<float>(static_cast<std::uint32_t>(value)))
      : std::to_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && output.append({text.data(),
      static_cast<std::size_t>(result.ptr - text.data())});
}

inline SqlFormatError append_escaped(SqlBuffer& output, const EscapeSnapshot& escape,
                                     std::string_view input) {
  if (input.size() > kSqlMaxSize - output.size()) return SqlFormatError::too_large;
  const auto capacity = input.size() * 2 + 1;
  std::array<char, kSqlInlineCapacity * 2 + 1> stack{};
  std::vector<char> heap;
  std::span<char> target(stack.data(), stack.size());
  if (capacity > target.size()) {
    heap.resize(capacity);
    target = heap;
  }
  std::size_t written = 0;
  if (!escape.escape(input, target, written) || written > capacity - 1)
    return SqlFormatError::escape_failed;
  return output.append({target.data(), written}) ? SqlFormatError::none : SqlFormatError::too_large;
}

template <typename ReadString>
inline SqlFormatResult format_sql(std::string_view format, std::span<const std::int32_t> args,
                                  ReadString&& read_string, const EscapeSnapshot* escape) {
  SqlFormatResult result;
  const auto required = sql_format_cells(format);
  if (!required) {
    result.error = SqlFormatError::malformed;
    return result;
  }
  result.expected = *required;
  result.available = args.size();
  if (args.size() < *required) {
    result.error = SqlFormatError::stack_underflow;
    return result;
  }

  std::size_t arg = 0;
  for (std::size_t i = 0; i < format.size(); ++i) {
    if (format[i] != '%') {
      if (!result.sql.append(format.substr(i, 1))) { result.error = SqlFormatError::too_large; return result; }
      continue;
    }
    const char specifier = format[++i];
    if (specifier == '%') {
      if (!result.sql.append("%")) { result.error = SqlFormatError::too_large; return result; }
      continue;
    }
    const std::int32_t value = args[arg++];
    bool ok = false;
    switch (specifier) {
      case 'd': case 'i': ok = append_number(result.sql, value, false); break;
      case 'f': ok = append_number(result.sql, value, true); break;
      case 'b': ok = result.sql.append(value ? "1" : "0"); break;
      case 's': case 'e': case 'I': {
        auto text = read_string(value);
        if (!text) { result.error = SqlFormatError::string_read; return result; }
        if (specifier == 's') {
          ok = result.sql.append(*text);
        } else if (specifier == 'e') {
          if (!escape) { result.error = SqlFormatError::escape_unavailable; return result; }
          result.error = append_escaped(result.sql, *escape, *text);
          if (result.error != SqlFormatError::none) return result;
          ok = true;
        } else {
          if (!escape) { result.error = SqlFormatError::escape_unavailable; return result; }
          const char delimiter = escape->backend == Backend::postgres ? '"' : '`';
          ok = result.sql.append({&delimiter, 1});
          for (const char ch : *text) {
            if (!ok) break;
            ok = result.sql.append({&ch, 1});
            if (ch == delimiter) ok = ok && result.sql.append({&ch, 1});
          }
          ok = ok && result.sql.append({&delimiter, 1});
        }
        break;
      }
      default: result.error = SqlFormatError::malformed; return result;
    }
    if (!ok) { result.error = SqlFormatError::too_large; return result; }
  }
  return result;
}

}  // namespace pawndb
