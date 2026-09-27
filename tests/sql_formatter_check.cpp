#include <pawndb/sql_formatter.hpp>

#include <bit>
#include <memory>
#include <string>
#include <unordered_map>

namespace {

struct FakePool final : pawndb::SessionPool {
  explicit FakePool(pawndb::Backend type) : backend(type) {}
  pawndb::Backend backend;
  bool query(std::string_view, pawndb::DriverError&) override { return true; }
  bool escape_string(std::string_view input, std::span<char> output,
                     std::size_t& written) const override {
    written = 0;
    for (char ch : input) {
      if (ch == '\0' || ch == '\\' || ch == '\'' || ch == '"') {
        if (written + 2 >= output.size()) return false;
        output[written++] = '\\';
        output[written++] = ch == '\0' ? '0' : ch;
      } else {
        if (written + 1 >= output.size()) return false;
        output[written++] = ch;
      }
    }
    return true;
  }
};

std::shared_ptr<pawndb::EscapeSnapshot> snapshot(pawndb::Backend backend) {
  return std::make_shared<pawndb::EscapeSnapshot>(
      pawndb::EscapeSnapshot{backend, std::make_shared<FakePool>(backend)});
}

}  // namespace

int main() {
  using namespace pawndb;
  const auto mysql = snapshot(Backend::mariadb);
  const auto postgres = snapshot(Backend::postgres);
  const std::unordered_map<std::int32_t, std::string> strings{
      {11, "raw"}, {12, "O'Reilly\\"}, {13, "col`name"}, {14, "col\"name"},
      {15, "ภาษาไทย"}, {16, std::string("nul\0tail", 8)}};
  const auto read = [&](std::int32_t address) -> std::optional<std::string_view> {
    const auto it = strings.find(address);
    return it == strings.end() ? std::nullopt : std::optional<std::string_view>(it->second);
  };
  const std::int32_t args[]{-7, 4, std::bit_cast<std::int32_t>(1.5f), 11, 12, 1};
  auto result = format_sql("SELECT %d,%i,%f,%s,'%e',%b,%%", args, read, mysql.get());
  if (!result || result.sql.view() != "SELECT -7,4,1.5,raw,'O\\'Reilly\\\\',1,%") return 1;
  const std::int32_t mysql_id[]{13};
  result = format_sql("%I", mysql_id, read, mysql.get());
  if (!result || result.sql.view() != "`col``name`") return 1;
  const std::int32_t pg_id[]{14};
  result = format_sql("%I", pg_id, read, postgres.get());
  if (!result || result.sql.view() != "\"col\"\"name\"") return 1;
  const std::int32_t thai[]{15};
  result = format_sql("'%e'", thai, read, postgres.get());
  if (!result || result.sql.view() != "'ภาษาไทย'") return 1;
  const std::int32_t nul[]{16};
  result = format_sql("%e", nul, read, mysql.get());
  if (!result || result.sql.view() != "nul\\0tail") return 1;

  int reads = 0;
  result = format_sql("%d %s", std::span<const std::int32_t>(args, 1),
      [&](std::int32_t) -> std::optional<std::string_view> { ++reads; return "bad"; }, mysql.get());
  if (result.error != SqlFormatError::stack_underflow || result.expected != 2 ||
      result.available != 1 || reads) return 1;
  if (sql_format_cells("%x") || sql_format_cells("trailing %")) return 1;

  const std::string inline_sql(kSqlInlineCapacity, 'x');
  result = format_sql(inline_sql, {}, [](std::int32_t) -> std::optional<std::string_view> {
    return std::nullopt;
  }, nullptr);
  if (!result || result.sql.size() != kSqlInlineCapacity) return 1;
  result = format_sql(inline_sql + "x", {}, [](std::int32_t) -> std::optional<std::string_view> {
    return std::nullopt;
  }, nullptr);
  if (!result || result.sql.size() != kSqlInlineCapacity + 1) return 1;
  result = format_sql(std::string(kSqlMaxSize, 'x'), {}, [](std::int32_t) -> std::optional<std::string_view> {
    return std::nullopt;
  }, nullptr);
  if (!result || result.sql.size() != kSqlMaxSize) return 1;
  result = format_sql(std::string(kSqlMaxSize + 1, 'x'), {}, [](std::int32_t) -> std::optional<std::string_view> {
    return std::nullopt;
  }, nullptr);
  if (result.error != SqlFormatError::too_large) return 1;

  const std::string expands(kSqlMaxSize / 2, '\'');
  const std::int32_t large[]{99};
  const auto read_large = [&](std::int32_t) -> std::optional<std::string_view> {
    return expands;
  };
  result = format_sql("%e", large, read_large, mysql.get());
  if (!result || result.sql.size() != kSqlMaxSize) return 1;
  const std::string too_large(kSqlMaxSize / 2 + 1, '\'');
  const auto read_too_large = [&](std::int32_t) -> std::optional<std::string_view> {
    return too_large;
  };
  result = format_sql("%e", large, read_too_large, mysql.get());
  if (result.error != SqlFormatError::too_large) return 1;
  return 0;
}
