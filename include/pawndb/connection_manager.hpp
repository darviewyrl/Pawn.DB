#pragma once

#include <pawndb/connection_config.hpp>
#include <pawndb/handle_registry.hpp>
#include <pawndb/lifecycle.hpp>
#include <pawndb/session_pool.hpp>

#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pawndb {

enum class ConnectionState { pending, connected, failed, closing };

struct Connection {
  explicit Connection(void* script) : owner(script) {}
  void* owner;
  std::atomic<ConnectionState> state{ConnectionState::pending};
  std::mutex mutex;
  std::optional<ConnectionConfig> config;
  std::optional<std::string> charset;
  std::optional<int> timeout;
  std::optional<int> debug_level;
  std::optional<bool> reconnect;
  std::optional<bool> multi_statements;
  std::shared_ptr<SessionPool> sessions;
  bool handshake_started = false;

  void set_config(ConnectionConfig next) {
    std::lock_guard lock(mutex);
    if (charset) next.charset = *charset;
    if (timeout) next.connect_timeout = *timeout;
    if (debug_level) next.debug_level = *debug_level;
    if (reconnect) next.auto_reconnect = *reconnect;
    if (multi_statements) next.multi_statements = *multi_statements;
    config = std::move(next);
  }
};

class ConnectionManager {
 public:
  using Handle = HandleRegistry::Handle;
  using ErrorSink = std::function<void(void*, Handle, int, std::string)>;
  using WarningSink = std::function<void(std::string)>;

  ConnectionManager(Lifecycle& life, ErrorSink error, WarningSink warning,
                    SessionFactory factory = {})
      : life_(life), errors_(std::move(error)), warnings_(std::move(warning)),
        factory_(std::move(factory)),
        handles_([this](Handle handle) {
          warnings_("[Pawn.DB Warning] Invalid connection handle (Handle: " +
                    std::to_string(static_cast<std::int32_t>(handle)) + ").");
        }) {}

  ~ConnectionManager() {
    std::vector<Handle> pending;
    for (const auto& [_, handles] : owners_)
      pending.insert(pending.end(), handles.begin(), handles.end());
    for (auto handle : pending) close(handle);
  }

  Handle connect(void* amx, ConnectionConfig config) {
    if (!amx || config.host.empty() || config.user.empty() || config.database.empty() ||
        config.charset.empty() || config.port < 1 || config.port > 65535)
      return 0;
    auto* pool = factory_ && config.backend == Backend::mariadb ? life_.worker_pool() : nullptr;
    if (factory_ && config.backend == Backend::mariadb && !pool) return 0;
    auto connection = std::make_shared<Connection>(amx);
    connection->set_config(std::move(config));
    auto handle = handles_.insert(connection);
    if (handle) owners_[amx].insert(handle);
    if (handle && factory_ && connection->config->backend == Backend::mariadb) {
      if (!pool->submit(0, WorkerPool::Priority::normal,
                        connect_work(pool, connection, life_.context(amx), handle,
                                     factory_, errors_, amx)))
        queue_error(pool, connection, life_.context(amx), handle, amx);
    }
    return handle;
  }

  Handle connect_file(void* amx, std::string path) {
    if (!amx || path.empty()) return 0;
    auto* pool = life_.worker_pool();
    if (!pool) return 0;
    auto connection = std::make_shared<Connection>(amx);
    auto handle = handles_.insert(connection);
    if (!handle) return 0;
    owners_[amx].insert(handle);
    auto context = life_.context(amx);
    if (!pool->submit(0, WorkerPool::Priority::normal,
                      [pool, connection, context, handle, path = std::move(path),
                       factory = factory_, errors = errors_, warnings = warnings_, amx] {
                        if (connection->state == ConnectionState::closing) return;
                        std::string message;
                        int code = -4;
                        auto config = load_config_file(path, message, &code);
                        if (config) {
                          try {
                            connection->set_config(std::move(*config));
                            Backend backend;
                            {
                              std::lock_guard lock(connection->mutex);
                              backend = connection->config->backend;
                            }
                            if (factory && backend == Backend::mariadb)
                              establish(pool, connection, context, handle, factory, errors, amx);
                            return;
                          } catch (const std::exception& exception) {
                            code = -4;
                            message = exception.what();
                          }
                        }
                        auto expected = ConnectionState::pending;
                        if (!connection->state.compare_exchange_strong(expected, ConnectionState::failed))
                          return;
                        pool->publish(Lifecycle::guard_callback(
                            context, [connection, handle, message = std::move(message),
                                      path, code, errors, warnings, amx] {
                              if (connection->state != ConnectionState::failed) return;
                              if (code == -1)
                                warnings("[Pawn.DB Warning] Configuration file '" + path +
                                         "' was not found. A default template has been generated. Please configure it.");
                              errors(amx, handle, code, message);
                            }));
                      })) {
      queue_error(pool, connection, context, handle, amx);
    }
    return handle;
  }

  bool close(Handle handle) {
    auto connection = handles_.get<Connection>(handle);
    if (!connection) return false;
    if (connection->state.exchange(ConnectionState::closing) == ConnectionState::closing)
      return false;
    owners_[connection->owner].erase(handle);
    const bool erased = handles_.erase<Connection>(handle);
    if (auto* pool = life_.active_worker_pool())
      pool->defer_cleanup(0, [connection] {
        std::lock_guard lock(connection->mutex);
        connection->sessions.reset();
      });
    return erased;
  }

  bool is_connected(Handle handle) const {
    auto connection = handles_.get<Connection>(handle);
    return connection && connection->state == ConnectionState::connected;
  }

  bool set_option(Handle handle, int option, std::string value) {
    auto connection = handles_.get<Connection>(handle);
    if (!connection || option != 0 || value.empty() || connection->state != ConnectionState::pending)
      return false;
    std::lock_guard lock(connection->mutex);
    if (connection->handshake_started || connection->state != ConnectionState::pending) return false;
    connection->charset = std::move(value);
    if (connection->config) connection->config->charset = *connection->charset;
    return true;
  }

  bool set_option_int(Handle handle, int option, int value) {
    auto connection = handles_.get<Connection>(handle);
    if (!connection || connection->state != ConnectionState::pending) return false;
    std::lock_guard lock(connection->mutex);
    if (connection->handshake_started || connection->state != ConnectionState::pending) return false;
    switch (option) {
      case 1:
        if (value < 1) return false;
        connection->timeout = value;
        if (connection->config) connection->config->connect_timeout = value;
        return true;
      case 2:
        connection->reconnect = value != 0;
        if (connection->config) connection->config->auto_reconnect = *connection->reconnect;
        return true;
      case 3:
        connection->multi_statements = value != 0;
        if (connection->config) connection->config->multi_statements = *connection->multi_statements;
        return true;
      default: return false;
    }
  }

  bool set_debug_level(Handle handle, int level) {
    auto connection = handles_.get<Connection>(handle);
    if (!connection || level < 0 || level > 3 || connection->state == ConnectionState::closing ||
        connection->state == ConnectionState::failed)
      return false;
    std::lock_guard lock(connection->mutex);
    connection->debug_level = level;
    if (connection->config) connection->config->debug_level = level;
    return true;
  }

  std::optional<std::string> driver_name(Handle handle) const {
    auto connection = handles_.get<Connection>(handle);
    if (!connection) return std::nullopt;
    std::lock_guard lock(connection->mutex);
    if (!connection->config) return std::nullopt;
    return connection->config->backend == Backend::postgres ? "PostgreSQL" : "MariaDB";
  }

  void detach(void* amx) {
    auto it = owners_.find(amx);
    if (it == owners_.end()) return;
    std::vector<Handle> pending(it->second.begin(), it->second.end());
    for (auto handle : pending) close(handle);
    owners_.erase(it);
  }

 private:
  Lifecycle& life_;
  ErrorSink errors_;
  WarningSink warnings_;
  SessionFactory factory_;
  HandleRegistry handles_;
  std::unordered_map<void*, std::unordered_set<Handle>> owners_;

  static void establish(WorkerPool* pool, const std::shared_ptr<Connection>& connection,
                        Lifecycle::Context context, Handle handle, const SessionFactory& factory,
                        const ErrorSink& errors, void* amx) {
    if (connection->state != ConnectionState::pending) return;
    DriverError error;
    std::shared_ptr<SessionPool> sessions;
    try {
      ConnectionConfig config;
      {
        std::lock_guard lock(connection->mutex);
        if (connection->state != ConnectionState::pending) return;
        connection->handshake_started = true;
        config = *connection->config;
      }
      sessions = factory(config, error);
    } catch (const std::exception& exception) {
      error = {-4, exception.what()};
    } catch (...) {
      error = {-4, "internal connection failure"};
    }
    if (sessions) {
      std::lock_guard lock(connection->mutex);
      if (connection->state == ConnectionState::pending) {
        connection->sessions = std::move(sessions);
        connection->state = ConnectionState::connected;
      }
      return;
    }
    auto expected = ConnectionState::pending;
    if (!connection->state.compare_exchange_strong(expected, ConnectionState::failed)) return;
    pool->publish(Lifecycle::guard_callback(
        context, [connection, handle, error = std::move(error), errors, amx] {
          if (connection->state == ConnectionState::failed)
            errors(amx, handle, error.code, error.message);
        }));
  }

  static Work connect_work(WorkerPool* pool, std::shared_ptr<Connection> connection,
                           Lifecycle::Context context, Handle handle, SessionFactory factory,
                           ErrorSink errors, void* amx) {
    return [pool, connection = std::move(connection), context, handle,
            factory = std::move(factory), errors = std::move(errors), amx] {
      establish(pool, connection, context, handle, factory, errors, amx);
    };
  }

  void queue_error(WorkerPool* pool, const std::shared_ptr<Connection>& connection,
                   Lifecycle::Context context, Handle handle, void* amx) {
    connection->state = ConnectionState::failed;
    if (pool) pool->publish(Lifecycle::guard_callback(
        context, [connection, errors = errors_, amx, handle] {
          if (connection->state == ConnectionState::failed)
            errors(amx, handle, -3, "worker queue is full");
        }));
  }
};

}  // namespace pawndb
