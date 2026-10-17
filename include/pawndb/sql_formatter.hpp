#pragma once

#include <pawndb/session_pool.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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

enum class SqlFormatError { none, malformed, stack_underflow, string_read, scalar_read, invalid_number, escape_unavailable,
                            escape_failed, too_large, allocation_failure };

struct SqlFormatResult {
  SqlBuffer sql;
  SqlFormatError error = SqlFormatError::none;
  std::size_t expected = 0;
  std::size_t available = 0;
  std::size_t invalid_json = 0;
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

inline std::optional<std::size_t> sql_specifier_cells(std::string_view format, std::size_t& i) {
  if (++i == format.size()) return std::nullopt;
  if (format[i] == '%') return 0;
  if (format[i] == 'v') {
    if (i + 1 < format.size() && (format[i + 1] == '3' || format[i + 1] == '4'))
      return static_cast<std::size_t>(format[++i] - '0');
    return 3;
  }
  return std::string_view("difseIbj").find(format[i]) != std::string_view::npos
             ? std::optional<std::size_t>(1) : std::nullopt;
}

inline std::optional<std::size_t> sql_format_cells(std::string_view format) {
  std::size_t cells = 0;
  for (std::size_t i = 0; i < format.size(); ++i) {
    if (format[i] != '%') continue;
    const auto count = sql_specifier_cells(format, i);
    if (!count) return std::nullopt;
    cells += *count;
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

template <typename ReadString, typename ReadJson = std::nullptr_t, typename ReadScalar = std::nullptr_t>
inline SqlFormatResult format_sql(std::string_view format, std::span<const std::int32_t> args,
                                  ReadString&& read_string, const EscapeSnapshot* escape,
                                  ReadJson&& read_json = nullptr, ReadScalar&& read_scalar = nullptr) {
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
  char quote = 0;
  bool line_comment = false, block_comment = false;
  bool escape_quote = false;
  std::string_view dollar_quote;
  const auto whitespace = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; };
  for (std::size_t i = 0; i < format.size(); ++i) {
    std::size_t closing_quote = std::string_view::npos;
    if (!quote && dollar_quote.empty() && !line_comment && !block_comment && format[i] == '\'') {
      auto start = i + 1;
      while (start < format.size() && whitespace(format[start])) ++start;
      if (start + 1 < format.size() && format[start] == '%' &&
          (format[start + 1] == 'j' || format[start + 1] == 'v')) {
        auto end = start;
        if (sql_specifier_cells(format, end)) {
          ++end;
          while (end < format.size() && whitespace(format[end])) ++end;
          if (end < format.size() && format[end] == '\'') {
            if (i && (format[i - 1] == 'E' || format[i - 1] == 'e')) {
              result.error = SqlFormatError::malformed; return result;
            }
            closing_quote = end; i = start;
          }
        }
      }
    }
    if (format[i] != '%') {
      const char ch = format[i];
      const char next = i + 1 < format.size() ? format[i + 1] : '\0';
      if (!dollar_quote.empty()) {
        if (format.substr(i).starts_with(dollar_quote)) {
          if (!result.sql.append(dollar_quote)) { result.error = SqlFormatError::too_large; return result; }
          i += dollar_quote.size() - 1; dollar_quote = {}; continue;
        }
      } else if (line_comment) { if (ch == '\n' || ch == '\r') line_comment = false; }
      else if (block_comment) {
        if (ch == '*' && next == '/') {
          if (!result.sql.append("*/")) { result.error = SqlFormatError::too_large; return result; }
          ++i; block_comment = false; continue;
        }
      } else if (quote) {
        const bool backslash = escape_quote || (escape && escape->backend == Backend::mariadb);
        if ((ch == quote && next == quote) || (backslash && ch == '\\' && next)) {
          if (!result.sql.append(format.substr(i, 2))) { result.error = SqlFormatError::too_large; return result; }
          ++i; continue;
        }
        if (ch == quote) quote = 0;
      } else if ((ch == '-' && next == '-') || (ch == '#' && escape && escape->backend == Backend::mariadb)) line_comment = true;
      else if (ch == '/' && next == '*') block_comment = true;
      else if (ch == '\'' || ch == '"' || ch == '`') {
        quote = ch;
        escape_quote = ch == '\'' && i && (format[i - 1] == 'e' || format[i - 1] == 'E');
      } else if (ch == '$' && escape && escape->backend == Backend::postgres) {
        auto end = i + 1;
        while (end < format.size() && ((format[end] >= 'a' && format[end] <= 'z') ||
               (format[end] >= 'A' && format[end] <= 'Z') || format[end] == '_' ||
               (end > i + 1 && format[end] >= '0' && format[end] <= '9'))) ++end;
        if (end < format.size() && format[end] == '$') {
          dollar_quote = format.substr(i, end - i + 1);
          if (!result.sql.append(dollar_quote)) { result.error = SqlFormatError::too_large; return result; }
          i = end; continue;
        }
      }
      if (!result.sql.append(format.substr(i, 1))) { result.error = SqlFormatError::too_large; return result; }
      continue;
    }
    const char specifier = format[++i];
    if (specifier == '%') {
      if (!result.sql.append("%")) { result.error = SqlFormatError::too_large; return result; }
      continue;
    }
    if (specifier == 'j' || specifier == 'v') {
      if (quote || !dollar_quote.empty() || line_comment || block_comment) { result.error = SqlFormatError::malformed; return result; }
      std::string payload;
      if (specifier == 'j') {
        std::optional<std::string> json;
        if constexpr (!std::is_same_v<ReadJson, std::nullptr_t>) json = read_json(args[arg++]);
        else {
          auto raw = read_string(args[arg++]);
          if (raw && nlohmann::json::accept(*raw)) json = std::string(*raw);
        }
        if (!json) {
          ++result.invalid_json;
          if (!result.sql.append("NULL")) { result.error = SqlFormatError::too_large; return result; }
          if (closing_quote != std::string_view::npos) i = closing_quote;
          continue;
        }
        payload = std::move(*json);
      } else {
        auto dimensions = std::size_t{3};
        if (i + 1 < format.size() && (format[i + 1] == '3' || format[i + 1] == '4'))
          dimensions = static_cast<std::size_t>(format[++i] - '0');
        for (std::size_t n = 0; n < dimensions; ++n) {
          std::optional<std::int32_t> value;
          if constexpr (std::is_same_v<ReadScalar, std::nullptr_t>) value = args[arg++];
          else value = read_scalar(args[arg++]);
          if (!value) { result.error = SqlFormatError::scalar_read; return result; }
          const auto number = std::bit_cast<float>(*value);
          if (!std::isfinite(number)) { result.error = SqlFormatError::invalid_number; return result; }
          std::array<char, 64> text{};
          const auto converted = std::to_chars(text.data(), text.data() + text.size(), number);
          if (converted.ec != std::errc{}) { result.error = SqlFormatError::invalid_number; return result; }
          if (n) payload += ',';
          payload.append(text.data(), static_cast<std::size_t>(converted.ptr - text.data()));
        }
      }
      if (!escape) { result.error = SqlFormatError::escape_unavailable; return result; }
      if (!result.sql.append("'")) { result.error = SqlFormatError::too_large; return result; }
      result.error = append_escaped(result.sql, *escape, payload);
      if (result.error != SqlFormatError::none) return result;
      if (!result.sql.append("'")) { result.error = SqlFormatError::too_large; return result; }
      if (closing_quote != std::string_view::npos) i = closing_quote;
      continue;
    }
    std::int32_t value = args[arg++];
    if constexpr (!std::is_same_v<ReadScalar, std::nullptr_t>) {
      if (std::string_view("difb").find(specifier) != std::string_view::npos) {
        const auto scalar = read_scalar(value);
        if (!scalar) { result.error = SqlFormatError::scalar_read; return result; }
        value = *scalar;
      }
    }
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
