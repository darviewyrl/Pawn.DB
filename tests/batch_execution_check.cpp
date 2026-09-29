#include <pawndb/session_pool.hpp>

#include <array>
#include <string>

int main() {
  const std::array<std::string, 5> atomic_statements = {"write 1", "write 2", "write 3",
                                                        "FAIL", "write 5"};
  const std::array<std::string, 5> non_atomic_statements = {"write 1", "write 2", "FAIL",
                                                            "write 4", "write 5"};
  int committed = 0;
  int transaction_start = 0;
  auto execute = [&](std::string_view sql, pawndb::QueryResult&, pawndb::DriverError& error) {
    if (sql == "BEGIN") { transaction_start = committed; return true; }
    if (sql == "ROLLBACK") { committed = transaction_start; return true; }
    if (sql == "COMMIT") return true;
    if (sql == "FAIL") { error = {1062, "duplicate key"}; return false; }
    committed += sql.back() - '0';
    return true;
  };

  const auto atomic = pawndb::run_batch_sequential(atomic_statements, true, execute);
  if (committed != 0 || atomic.items.size() != 5 ||
      atomic.items[0].status != pawndb::BatchItemStatus::rolled_back ||
      atomic.items[1].status != pawndb::BatchItemStatus::rolled_back ||
      atomic.items[2].status != pawndb::BatchItemStatus::rolled_back ||
      atomic.items[3].status != pawndb::BatchItemStatus::sql_error ||
      atomic.items[4].status != pawndb::BatchItemStatus::not_executed) return 1;

  committed = 0;
  const auto non_atomic = pawndb::run_batch_sequential(non_atomic_statements, false, execute);
  if (committed != 12 || non_atomic.items.size() != 5 ||
      non_atomic.items[0].status != pawndb::BatchItemStatus::success ||
      non_atomic.items[1].status != pawndb::BatchItemStatus::success ||
      non_atomic.items[2].status != pawndb::BatchItemStatus::sql_error ||
      non_atomic.items[3].status != pawndb::BatchItemStatus::success ||
      non_atomic.items[4].status != pawndb::BatchItemStatus::success ||
      non_atomic.items[2].error.message != "duplicate key") return 2;

  auto transport = [&](std::string_view sql, pawndb::QueryResult&, pawndb::DriverError& error) {
    if (sql == "write 1") { committed += 1; return true; }
    error = {2013, "connection lost", true};
    return false;
  };
  const auto disconnected = pawndb::run_batch_sequential(non_atomic_statements, false, transport);
  if (!disconnected.connection_error ||
      disconnected.items[0].status != pawndb::BatchItemStatus::success ||
      disconnected.items[1].status != pawndb::BatchItemStatus::sql_error ||
      disconnected.items[2].status != pawndb::BatchItemStatus::not_executed ||
      disconnected.items[4].status != pawndb::BatchItemStatus::not_executed) return 3;
}
