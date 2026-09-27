#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pawndb {

constexpr int kPostgresNoSqlstate = 60466177;

inline int encode_sqlstate(std::string_view state) {
  if (state.size() != 5) return kPostgresNoSqlstate;
  int code = 1;
  for (char value : state) {
    const int digit = value >= '0' && value <= '9' ? value - '0' :
                      value >= 'A' && value <= 'Z' ? value - 'A' + 10 : -1;
    if (digit < 0) return kPostgresNoSqlstate;
    code = (code - 1) * 36 + digit + 1;
  }
  return code;
}

inline std::optional<std::string> decode_sqlstate(int code) {
  if (code < 1 || code >= kPostgresNoSqlstate) return std::nullopt;
  std::string state(5, '0');
  --code;
  for (int i = 4; i >= 0; --i) {
    const int digit = code % 36;
    state[i] = digit < 10 ? static_cast<char>('0' + digit) :
                            static_cast<char>('A' + digit - 10);
    code /= 36;
  }
  return state;
}

}  // namespace pawndb
