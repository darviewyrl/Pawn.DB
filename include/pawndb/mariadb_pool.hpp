#pragma once

#include <pawndb/session_pool.hpp>
#include <mysql.h>

#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace pawndb {

class MariaPool final : public SessionPool {
 public:
  ~MariaPool() override {
    for (auto* session : sessions_) mysql_close(session);
    if (escape_session_) mysql_close(escape_session_);
  }

  static std::shared_ptr<SessionPool> open(const ConnectionConfig& config, DriverError& error) {
    auto pool = std::shared_ptr<MariaPool>(new MariaPool);
    const auto timeout = static_cast<unsigned int>(config.connect_timeout);
    const bool ssl_enforce = config.ssl_enabled;
    const bool verify_server = config.verify_server_cert;
    const char* tls_versions = "TLSv1.2,TLSv1.3";
    for (int i = 0; i < config.pool_size; ++i) {
      std::unique_ptr<MYSQL, decltype(&mysql_close)> session(mysql_init(nullptr), mysql_close);
      if (!session) {
        error = {-4, "MariaDB Connector/C initialization failed"};
        return {};
      }
      const auto flags = config.multi_statements ? CLIENT_MULTI_STATEMENTS : 0;
      if (mysql_options(session.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout) ||
          mysql_options(session.get(), MYSQL_SET_CHARSET_NAME, config.charset.c_str()) ||
          (config.ssl_enabled &&
           (mysql_ssl_set(session.get(), config.client_key.empty() ? nullptr : config.client_key.c_str(),
                          config.client_cert.empty() ? nullptr : config.client_cert.c_str(),
                          config.ca_cert.c_str(), nullptr, nullptr) ||
            mysql_options(session.get(), MYSQL_OPT_SSL_ENFORCE, &ssl_enforce) ||
            mysql_options(session.get(), MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify_server) ||
            mysql_options(session.get(), MYSQL_OPT_TLS_VERSION, tls_versions))) ||
          !mysql_real_connect(session.get(), config.host.c_str(), config.user.c_str(),
                              config.password.c_str(), config.database.c_str(),
                              static_cast<unsigned int>(config.port), nullptr, flags)) {
        error = {static_cast<int>(mysql_errno(session.get())), mysql_error(session.get())};
        if (!error.code) error.code = -4;
        return {};
      }
      pool->sessions_.push_back(session.get());
      session.release();
      pool->busy_.push_back(false);
    }
    std::unique_ptr<MYSQL, decltype(&mysql_close)> escape(mysql_init(nullptr), mysql_close);
    const auto flags = config.multi_statements ? CLIENT_MULTI_STATEMENTS : 0;
    if (!escape || mysql_options(escape.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout) ||
        mysql_options(escape.get(), MYSQL_SET_CHARSET_NAME, config.charset.c_str()) ||
        (config.ssl_enabled &&
         (mysql_ssl_set(escape.get(), config.client_key.empty() ? nullptr : config.client_key.c_str(),
                        config.client_cert.empty() ? nullptr : config.client_cert.c_str(),
                        config.ca_cert.c_str(), nullptr, nullptr) ||
          mysql_options(escape.get(), MYSQL_OPT_SSL_ENFORCE, &ssl_enforce) ||
          mysql_options(escape.get(), MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify_server) ||
          mysql_options(escape.get(), MYSQL_OPT_TLS_VERSION, tls_versions))) ||
        !mysql_real_connect(escape.get(), config.host.c_str(), config.user.c_str(),
                            config.password.c_str(), config.database.c_str(),
                            static_cast<unsigned int>(config.port), nullptr, flags)) {
      error = {escape ? static_cast<int>(mysql_errno(escape.get())) : -4,
               escape ? mysql_error(escape.get()) : "MariaDB escape connection initialization failed"};
      if (!error.code) error.code = -4;
      return {};
    }
    pool->escape_session_ = escape.release();
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

    bool ok = mysql_real_query(session, sql.data(), static_cast<unsigned long>(sql.size())) == 0;
    if (ok) {
      for (;;) {
        if (auto* result = mysql_store_result(session)) mysql_free_result(result);
        else if (mysql_field_count(session)) { ok = false; break; }
        if (!mysql_more_results(session)) break;
        if (mysql_next_result(session)) { ok = false; break; }
      }
    }
    if (!ok) {
      error = {static_cast<int>(mysql_errno(session)), mysql_error(session)};
      if (!error.code) error.code = -4;
    }
    lock.lock();
    busy_[index] = false;
    lock.unlock();
    cv_.notify_one();
    return ok;
  }

  bool escape_string(std::string_view input, std::span<char> output,
                     std::size_t& written) const override {
    if (!escape_session_ || input.size() > (std::numeric_limits<unsigned long>::max() - 1) / 2 ||
        output.size() < input.size() * 2 + 1) return false;
    const auto* data = input.empty() ? "" : input.data();
    written = mysql_real_escape_string(escape_session_, output.data(), data,
                                       static_cast<unsigned long>(input.size()));
    return written < output.size();
  }

 private:
  MariaPool() = default;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<MYSQL*> sessions_;
  std::vector<bool> busy_;
  MYSQL* escape_session_ = nullptr;
};

}  // namespace pawndb
