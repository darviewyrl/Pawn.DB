#pragma once

#include <pawndb/session_pool.hpp>
#include <pawndb/sqlstate.hpp>
#include <libpq-fe.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace pawndb {

class PostgresPool final : public SessionPool {
 public:
  ~PostgresPool() override {
    for (auto* session : sessions_) PQfinish(session);
  }

  static std::shared_ptr<SessionPool> open(const ConnectionConfig& config, DriverError& error) {
    if (config.ssl_enabled) {
      error = {-4, "TLS connection settings are not supported by this build"};
      return {};
    }
    auto pool = std::shared_ptr<PostgresPool>(new PostgresPool);
    pool->multi_statements_ = config.multi_statements;
    const auto port = std::to_string(config.port);
    const auto timeout = std::to_string(config.connect_timeout);
    const char* keys[] = {"host", "port", "user", "password", "dbname", "connect_timeout",
                          "client_encoding", "sslmode", nullptr};
    const char* values[] = {config.host.c_str(), port.c_str(), config.user.c_str(),
                            config.password.c_str(), config.database.c_str(), timeout.c_str(),
                            config.charset == "utf8mb4" ? "UTF8" : config.charset.c_str(),
                            "disable", nullptr};
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
    return pool;
  }

  bool query(std::string_view sql, DriverError& error) override {
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

    const std::string statement(sql);
    std::unique_ptr<PGresult, decltype(&PQclear)> result(
        multi_statements_ ? PQexec(session, statement.c_str()) :
                            PQexecParams(session, statement.c_str(), 0, nullptr, nullptr,
                                         nullptr, nullptr, 0), PQclear);
    const bool ok = result && (PQresultStatus(result.get()) == PGRES_COMMAND_OK ||
                               PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    if (!ok) {
      const char* state = result ? PQresultErrorField(result.get(), PG_DIAG_SQLSTATE) : nullptr;
      error.code = state ? encode_sqlstate(state) : kPostgresNoSqlstate;
      error.message = state ? "[" + std::string(state) + "] " + PQresultErrorMessage(result.get()) :
                              PQerrorMessage(session);
    }
    lock.lock();
    busy_[index] = false;
    lock.unlock();
    cv_.notify_one();
    return ok;
  }

 private:
  PostgresPool() = default;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<PGconn*> sessions_;
  std::vector<bool> busy_;
  bool multi_statements_ = false;
};

}  // namespace pawndb
