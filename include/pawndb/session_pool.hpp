#pragma once

#include <pawndb/connection_config.hpp>
#include <pawndb/result.hpp>

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pawndb {

struct DriverError {
  int code = -4;
  std::string message;
  bool connection_error = false;
  bool fatal_error = false;
};

struct BatchStatementResult {
  BatchItemStatus status = BatchItemStatus::not_executed;
  QueryResult result;
  DriverError error;
};

struct BatchExecutionResult {
  std::vector<BatchStatementResult> items;
  bool connection_error = false;
};

template <class Execute>
BatchExecutionResult run_batch_sequential(std::span<const std::string> statements, bool atomic,
                                          Execute&& execute) {
  BatchExecutionResult batch;
  batch.items.resize(statements.size());
  if (statements.empty()) return batch;
  auto run = [&](std::string_view sql, QueryResult& result, DriverError& error) {
    try { return execute(sql, result, error); }
    catch (const std::exception& exception) { error = {-4, exception.what(), false, true}; }
    catch (...) { error = {-4, "internal batch execution failure", false, true}; }
    return false;
  };
  if (atomic) {
    QueryResult ignored;
    DriverError error;
    if (!run("BEGIN", ignored, error)) {
      batch.items.front().status = BatchItemStatus::sql_error;
      batch.items.front().error = std::move(error);
      batch.connection_error = batch.items.front().error.connection_error;
      for (std::size_t i = 1; i < batch.items.size(); ++i)
        batch.items[i].status = BatchItemStatus::not_executed;
      return batch;
    }
  }
  for (std::size_t i = 0; i < statements.size(); ++i) {
    auto& item = batch.items[i];
    if (run(statements[i], item.result, item.error)) {
      item.status = BatchItemStatus::success;
      continue;
    }
    item.status = BatchItemStatus::sql_error;
    batch.connection_error = item.error.connection_error;
    if (atomic) {
      QueryResult ignored;
      DriverError rollback_error;
      run("ROLLBACK", ignored, rollback_error);
      batch.connection_error = batch.connection_error || rollback_error.connection_error;
      for (std::size_t previous = 0; previous < i; ++previous)
        batch.items[previous].status = BatchItemStatus::rolled_back;
      for (std::size_t remaining = i + 1; remaining < batch.items.size(); ++remaining)
        batch.items[remaining].status = BatchItemStatus::not_executed;
      return batch;
    }
    if (item.error.connection_error || item.error.fatal_error) {
      for (std::size_t remaining = i + 1; remaining < batch.items.size(); ++remaining)
        batch.items[remaining].status = BatchItemStatus::not_executed;
      return batch;
    }
  }
  if (atomic) {
    QueryResult ignored;
    DriverError error;
    if (!run("COMMIT", ignored, error)) {
      batch.connection_error = error.connection_error;
      auto& last = batch.items.back();
      last.status = BatchItemStatus::sql_error;
      last.error = std::move(error);
      for (std::size_t previous = 0; previous + 1 < batch.items.size(); ++previous)
        batch.items[previous].status = BatchItemStatus::rolled_back;
      QueryResult rollback_result;
      DriverError rollback_error;
      run("ROLLBACK", rollback_result, rollback_error);
      batch.connection_error = batch.connection_error || rollback_error.connection_error;
    }
  }
  return batch;
}

class SessionPool {
 public:
  virtual ~SessionPool() = default;
  virtual bool query(std::string_view sql, DriverError& error) = 0;
  virtual bool query_result(std::string_view sql, DriverError& error, QueryResult& result) {
    result = {};
    return query(sql, error);
  }
  virtual BatchExecutionResult execute_batch(std::span<const std::string> statements,
                                             bool atomic) {
    return run_batch_sequential(statements, atomic, [this](std::string_view sql,
        QueryResult& result, DriverError& error) { return query_result(sql, error, result); });
  }
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
