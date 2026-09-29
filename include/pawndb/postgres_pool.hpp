#pragma once

#include <pawndb/session_pool.hpp>
#include <pawndb/sqlstate.hpp>
#include <libpq-fe.h>

#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace pawndb {

class PostgresPool final : public SessionPool {
 public:
  ~PostgresPool() override {
    for (auto* session : sessions_) PQfinish(session);
    if (escape_session_) PQfinish(escape_session_);
  }

  static std::shared_ptr<SessionPool> open(const ConnectionConfig& config, DriverError& error) {
    auto pool = std::shared_ptr<PostgresPool>(new PostgresPool);
    pool->multi_statements_ = config.multi_statements;
    const auto port = std::to_string(config.port);
    const auto timeout = std::to_string(config.connect_timeout);
    const char* keys[] = {"host", "port", "user", "password", "dbname", "connect_timeout",
                          "client_encoding", "sslmode", "sslrootcert", "sslcert", "sslkey",
                          "ssl_min_protocol_version", "ssl_max_protocol_version", nullptr};
    const char* sslmode = !config.ssl_enabled ? "disable" :
        (config.verify_server_cert ? "verify-full" : "require");
    const char* values[] = {config.host.c_str(), port.c_str(), config.user.c_str(),
                            config.password.c_str(), config.database.c_str(), timeout.c_str(),
                            config.charset == "utf8mb4" ? "UTF8" : config.charset.c_str(),
                            sslmode, config.ssl_enabled && config.verify_server_cert
                                ? config.ca_cert.c_str() : nullptr,
                            config.client_cert.empty() ? nullptr : config.client_cert.c_str(),
                            config.client_key.empty() ? nullptr : config.client_key.c_str(),
                            config.ssl_enabled ? "TLSv1.2" : nullptr,
                            config.ssl_enabled ? "TLSv1.3" : nullptr, nullptr};
    for (int i = 0; i < config.pool_size; ++i) {
      std::unique_ptr<PGconn, decltype(&PQfinish)> session(PQconnectdbParams(keys, values, 0),
                                                           PQfinish);
      if (!session) {
        error = {-4, "libpq initialization failed"};
        return {};
      }
      if (PQstatus(session.get()) != CONNECTION_OK) {
        error = {kPostgresNoSqlstate, PQerrorMessage(session.get())};
        return {};
      }
      pool->sessions_.push_back(session.get());
      session.release();
      pool->busy_.push_back(false);
    }
    std::unique_ptr<PGconn, decltype(&PQfinish)> escape(PQconnectdbParams(keys, values, 0), PQfinish);
    if (!escape || PQstatus(escape.get()) != CONNECTION_OK) {
      error = {kPostgresNoSqlstate, escape ? PQerrorMessage(escape.get()) :
                                               "PostgreSQL escape connection initialization failed"};
      return {};
    }
    pool->escape_session_ = escape.release();
    return pool;
  }

  bool query(std::string_view sql, DriverError& error) override {
    return execute(sql, error, nullptr);
  }

  bool query_result(std::string_view sql, DriverError& error, QueryResult& output) override {
    return execute(sql, error, &output);
  }

  BatchExecutionResult execute_batch(std::span<const std::string> statements,
                                     bool atomic) override {
    const auto [session, index] = acquire_session();
    SessionLease lease{mutex_, cv_, busy_, index};
    return run_batch_sequential(statements, atomic,
        [this, session](std::string_view sql, QueryResult& result, DriverError& error) {
          return execute_on_session(session, sql, error, &result);
        });
  }

 private:
  bool execute(std::string_view sql, DriverError& error, QueryResult* output) {
    const auto [session, index] = acquire_session();
    SessionLease lease{mutex_, cv_, busy_, index};
    return execute_on_session(session, sql, error, output);
  }

  std::pair<PGconn*, std::size_t> acquire_session() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] {
      for (bool busy : busy_) if (!busy) return true;
      return false;
    });
    std::size_t index = 0;
    while (busy_[index]) ++index;
    busy_[index] = true;
    auto* session = sessions_[index];
    lock.unlock();
    return {session, index};
  }

  bool execute_on_session(PGconn* session, std::string_view sql, DriverError& error,
                          QueryResult* output) const {

    const std::string statement(sql);
    std::unique_ptr<PGresult, decltype(&PQclear)> result(
        multi_statements_ ? PQexec(session, statement.c_str()) :
                            PQexecParams(session, statement.c_str(), 0, nullptr, nullptr,
                                         nullptr, nullptr, 0), PQclear);
    const bool ok = result && (PQresultStatus(result.get()) == PGRES_COMMAND_OK ||
                               PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    if (output) {
      *output = {};
      if (ok && PQresultStatus(result.get()) == PGRES_TUPLES_OK) {
        try {
          const auto fields = PQnfields(result.get());
          const auto rows = PQntuples(result.get());
          output->fields.reserve(fields);
          output->rows.reserve(rows);
          for (int field = 0; field < fields; ++field)
            output->fields.emplace_back(PQfname(result.get(), field));
          for (int row = 0; row < rows; ++row) {
            std::vector<std::optional<std::string>> values;
            values.reserve(fields);
            for (int field = 0; field < fields; ++field)
              values.emplace_back(PQgetisnull(result.get(), row, field)
                  ? std::nullopt
                  : std::optional<std::string>(std::string(PQgetvalue(result.get(), row, field),
                                                           PQgetlength(result.get(), row, field))));
            output->rows.push_back(std::move(values));
          }
        } catch (...) {
          *output = {};
          error = {-4, "result allocation failed", false, true};
          return false;
        }
      }
    }
    if (!ok) {
      const char* state = result ? PQresultErrorField(result.get(), PG_DIAG_SQLSTATE) : nullptr;
      error.code = state ? encode_sqlstate(state) : kPostgresNoSqlstate;
      error.message = state ? "[" + std::string(state) + "] " + PQresultErrorMessage(result.get()) :
                              PQerrorMessage(session);
      error.connection_error = PQstatus(session) != CONNECTION_OK || !result ||
          (state && state[0] == '0' && state[1] == '8');
    }
    return ok;
  }

  struct SessionLease {
    std::mutex& mutex;
    std::condition_variable& cv;
    std::vector<bool>& busy;
    std::size_t index;
    ~SessionLease() {
      { std::lock_guard lock(mutex); busy[index] = false; }
      cv.notify_one();
    }
  };

 public:
  bool escape_string(std::string_view input, std::span<char> output,
                     std::size_t& written) const override {
    if (!escape_session_ || input.size() > (std::numeric_limits<std::size_t>::max() - 1) / 2 ||
        output.size() < input.size() * 2 + 1) return false;
    int error = 0;
    const auto* data = input.empty() ? "" : input.data();
    written = PQescapeStringConn(escape_session_, output.data(), data, input.size(), &error);
    return error == 0 && written < output.size();
  }

 private:
  PostgresPool() = default;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<PGconn*> sessions_;
  std::vector<bool> busy_;
  bool multi_statements_ = false;
  PGconn* escape_session_ = nullptr;
};

}  // namespace pawndb
