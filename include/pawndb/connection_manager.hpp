#pragma once

#include <pawndb/connection_config.hpp>
#include <pawndb/handle_registry.hpp>
#include <pawndb/lifecycle.hpp>
#include <pawndb/session_pool.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pawndb {

enum class ConnectionState { pending, connected, disconnected, reconnecting, failed, closing };
inline constexpr auto kMaxReconnectDelay = std::chrono::seconds(15);
inline constexpr std::chrono::seconds next_reconnect_delay(std::chrono::seconds current) {
  const auto next = current * 2;
  return next > kMaxReconnectDelay ? kMaxReconnectDelay : next;
}

struct PendingQuery {
  std::string sql;
  std::function<void(bool, DriverError)> completion;
  Lifecycle::Context context;
  void* amx;
  std::uint32_t handle;
};

struct Connection {
  explicit Connection(void* script) : owner(script) {}
  void* owner;
  std::atomic<ConnectionState> state{ConnectionState::pending};
  std::mutex mutex;
  std::optional<ConnectionConfig> config;
  std::optional<int> debug_level;
  std::shared_ptr<SessionPool> sessions;
  std::atomic<std::shared_ptr<const EscapeSnapshot>> escape_context;
  std::deque<PendingQuery> pending_queries;
  bool draining_queries = false;

  void set_config(ConnectionConfig next) {
    std::lock_guard lock(mutex);
    if (debug_level) next.debug_level = *debug_level;
    config = std::move(next);
  }
};

struct SetupConfiguration {
  explicit SetupConfiguration(void* script) : owner(script) {}
  void* owner;
  std::mutex mutex;
  ConnectionConfig config;
  DriverChoice driver = DriverChoice::automatic;
};

class ConnectionManager {
 public:
  using Handle = HandleRegistry::Handle;
  using QueryCompletion = std::function<void(bool, DriverError)>;
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
    std::vector<void*> scripts;
    scripts.reserve(owners_.size());
    for (const auto& [script, _] : owners_) scripts.push_back(script);
    for (auto* script : scripts) detach(script);
  }

  Handle connect(void* amx, ConnectionConfig config) {
    return connect(amx, std::move(config), 0, true);
  }

  Handle connect(void* amx, ConnectionConfig config, Handle setup_handle, bool auto_free_setup) {
    if (setup_handle) {
      auto setup = handles_.get<SetupConfiguration>(setup_handle);
      if (!setup) return 0;
      {
        std::lock_guard lock(setup->mutex);
        config.charset = setup->config.charset;
        config.connect_timeout = setup->config.connect_timeout;
        config.auto_reconnect = setup->config.auto_reconnect;
        config.multi_statements = setup->config.multi_statements;
        config.pool_size = setup->config.pool_size;
        config.ssl_enabled = setup->config.ssl_enabled;
        config.ca_cert = setup->config.ca_cert;
        config.client_cert = setup->config.client_cert;
        config.client_key = setup->config.client_key;
        config.verify_server_cert = setup->config.verify_server_cert;
        config.backend = setup->driver == DriverChoice::automatic ? infer_backend(config.port) :
            (setup->driver == DriverChoice::postgres ? Backend::postgres : Backend::mariadb);
      }
      if (auto_free_setup) free_setup(setup_handle);
    } else {
      config.backend = infer_backend(config.port);
    }
    if (!amx || config.host.empty() || config.user.empty() || config.database.empty() ||
        config.charset.empty() || config.port < 1 || config.port > 65535 ||
        config.connect_timeout < 1 || config.pool_size < 1 ||
        (config.ssl_enabled && (config.ca_cert.empty() ||
         config.client_cert.empty() != config.client_key.empty())))
      return 0;
    auto* pool = factory_ ? life_.worker_pool() : nullptr;
    if (factory_ && !pool) return 0;
    auto connection = std::make_shared<Connection>(amx);
    connection->set_config(std::move(config));
    auto handle = handles_.insert(connection);
    if (handle) owners_[amx].insert(handle);
    if (handle && factory_) {
      if (!pool->submit(0, WorkerPool::Priority::normal,
                        connect_work(pool, connection, life_.context(amx), handle,
                                     factory_, errors_, amx)))
        queue_error(pool, connection, life_.context(amx), handle, amx);
    }
    return handle;
  }

  Handle setup_init(void* amx) {
    if (!amx) return 0;
    auto setup = std::make_shared<SetupConfiguration>(amx);
    const auto handle = handles_.insert(setup);
    if (handle) owners_[amx].insert(handle);
    return handle;
  }

  bool free_setup(Handle handle) {
    auto setup = handles_.get<SetupConfiguration>(handle);
    if (!setup || !handles_.erase<SetupConfiguration>(handle)) return false;
    owners_[setup->owner].erase(handle);
    return true;
  }

  bool setup_charset(Handle handle, std::string value) {
    auto setup = handles_.get<SetupConfiguration>(handle);
    if (!setup || value.empty()) return false;
    std::lock_guard lock(setup->mutex);
    setup->config.charset = std::move(value);
    return true;
  }

  bool setup_option(Handle handle, int option, int value) {
    auto setup = handles_.get<SetupConfiguration>(handle);
    if (!setup) return false;
    std::lock_guard lock(setup->mutex);
    switch (option) {
      case 0:
        if (value < 1) return false;
        setup->config.connect_timeout = value;
        return true;
      case 1: setup->config.auto_reconnect = value != 0; return true;
      case 2: setup->config.multi_statements = value != 0; return true;
      case 3:
        if (value < 1) return false;
        setup->config.pool_size = value;
        return true;
      default: return false;
    }
  }

  bool setup_driver(Handle handle, int driver) {
    auto setup = handles_.get<SetupConfiguration>(handle);
    if (!setup || driver < 0 || driver > 2) return false;
    std::lock_guard lock(setup->mutex);
    setup->driver = static_cast<DriverChoice>(driver);
    return true;
  }

  bool setup_ssl(Handle handle, std::string ca_cert, std::string client_cert,
                 std::string client_key, bool verify_server) {
    auto setup = handles_.get<SetupConfiguration>(handle);
    if (!setup || ca_cert.empty() || client_cert.empty() != client_key.empty()) return false;
    std::lock_guard lock(setup->mutex);
    setup->config.ssl_enabled = true;
    setup->config.ca_cert = std::move(ca_cert);
    setup->config.client_cert = std::move(client_cert);
    setup->config.client_key = std::move(client_key);
    setup->config.verify_server_cert = verify_server;
    return true;
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
                            if (factory)
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
    connection->escape_context.store({}, std::memory_order_release);
    if (auto* pool = life_.active_worker_pool())
      pool->defer_cleanup(0, [connection] {
        std::lock_guard lock(connection->mutex);
        connection->sessions.reset();
      });
    return erased;
  }

  bool query(Handle handle, std::string sql, QueryCompletion completion = {}) {
    auto connection = handles_.get<Connection>(handle);
    if (!connection || sql.empty()) return false;
    auto* pool = life_.active_worker_pool();
    if (!pool) return false;
    bool start_drain = false;
    {
      std::lock_guard lock(connection->mutex);
      const auto state = connection->state.load();
      if (state == ConnectionState::closing || state == ConnectionState::failed ||
          connection->pending_queries.size() >= 8192 ||
          (state != ConnectionState::connected && connection->config &&
           !connection->config->auto_reconnect)) return false;
      connection->pending_queries.push_back(
          {std::move(sql), std::move(completion), life_.context(connection->owner),
           connection->owner, handle});
      if (state == ConnectionState::connected && !connection->draining_queries) {
        connection->draining_queries = true;
        start_drain = true;
      }
    }
    if (start_drain && !pool->submit(0, WorkerPool::Priority::normal,
                                      [pool, connection] { drain_queries(pool, connection); })) {
      std::lock_guard lock(connection->mutex);
      connection->draining_queries = false;
    }
    return true;
  }

  bool is_connected(Handle handle) const {
    auto connection = handles_.get<Connection>(handle);
    return connection && connection->state == ConnectionState::connected;
  }

  std::shared_ptr<const EscapeSnapshot> escape_snapshot(Handle handle) const {
    auto connection = handles_.get<Connection>(handle);
    return connection ? connection->escape_context.load(std::memory_order_acquire) : nullptr;
  }

  void report_format_error(std::string message) const { warnings_(std::move(message)); }

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
    for (auto handle : pending) {
      if (handles_.get<SetupConfiguration>(handle, false)) free_setup(handle);
      else close(handle);
    }
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
        config = *connection->config;
      }
      sessions = factory(config, error);
    } catch (const std::exception& exception) {
      error = {-4, exception.what()};
    } catch (...) {
      error = {-4, "internal connection failure"};
    }
    if (sessions) {
      bool established = false;
      {
        std::lock_guard lock(connection->mutex);
        if (connection->state == ConnectionState::pending) {
          connection->sessions = sessions;
          connection->escape_context.store(make_escape_snapshot(pool, connection->config->backend,
                                                                 sessions),
                                           std::memory_order_release);
          connection->state = ConnectionState::connected;
          established = true;
        }
      }
      if (!established) return;
      schedule_health(pool, connection, factory, context, handle, amx,
                      std::chrono::seconds(5), std::chrono::seconds(1));
      schedule_drain(pool, connection);
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

  static void schedule_drain(WorkerPool* pool, const std::shared_ptr<Connection>& connection) {
    bool start = false;
    {
      std::lock_guard lock(connection->mutex);
      if (connection->state == ConnectionState::connected && !connection->pending_queries.empty() &&
          !connection->draining_queries) {
        connection->draining_queries = true;
        start = true;
      }
    }
    if (start && !pool->submit(0, WorkerPool::Priority::normal,
                               [pool, connection] { drain_queries(pool, connection); })) {
      std::lock_guard lock(connection->mutex);
      connection->draining_queries = false;
    }
  }

  static void complete_query(WorkerPool* pool, PendingQuery query, bool success,
                             DriverError error = {}) {
    if (!query.completion) return;
    pool->publish(Lifecycle::guard_callback(
        query.context, [completion = std::move(query.completion), success,
                        error = std::move(error)]() mutable {
          completion(success, std::move(error));
        }));
  }

  static void drain_queries(WorkerPool* pool, const std::shared_ptr<Connection>& connection) {
    for (;;) {
      PendingQuery query;
      std::shared_ptr<SessionPool> sessions;
      {
        std::lock_guard lock(connection->mutex);
        if (connection->state != ConnectionState::connected || connection->pending_queries.empty()) {
          connection->draining_queries = false;
          return;
        }
        query = std::move(connection->pending_queries.front());
        connection->pending_queries.pop_front();
        sessions = connection->sessions;
      }
      DriverError error;
      bool ok = false;
      try { ok = sessions && sessions->query(query.sql, error); }
      catch (const std::exception& exception) { error = {-4, exception.what()}; }
      catch (...) { error = {-4, "internal query failure"}; }
      if (!ok) {
        DriverError ping_error;
        bool healthy = false;
        try { healthy = sessions && sessions->ping(ping_error); }
        catch (...) {}
        if (!healthy) {
          std::lock_guard lock(connection->mutex);
          if (connection->state == ConnectionState::connected) {
            connection->state = ConnectionState::disconnected;
            connection->sessions.reset();
          }
        }
        complete_query(pool, std::move(query), false, std::move(error));
        if (connection->state != ConnectionState::connected) {
          std::lock_guard lock(connection->mutex);
          connection->draining_queries = false;
          return;
        }
      } else {
        complete_query(pool, std::move(query), true);
      }
    }
  }

  static void schedule_health(WorkerPool* pool, const std::shared_ptr<Connection>& connection,
                              SessionFactory factory, Lifecycle::Context context, Handle handle,
                              void* amx, std::chrono::seconds interval,
                              std::chrono::seconds retry) {
    std::weak_ptr<Connection> weak = connection;
    pool->schedule(0, interval, [pool, weak, factory = std::move(factory), context, handle,
                                 amx, retry]() mutable {
      auto connection = weak.lock();
      if (!connection) return;
      ConnectionConfig config;
      std::shared_ptr<SessionPool> sessions;
      bool reconnect = false;
      {
        std::lock_guard lock(connection->mutex);
        if (connection->state == ConnectionState::closing ||
            connection->state == ConnectionState::failed || !connection->config) return;
        config = *connection->config;
        if (connection->state == ConnectionState::connected) sessions = connection->sessions;
        else if ((connection->state == ConnectionState::disconnected ||
                  connection->state == ConnectionState::reconnecting) && config.auto_reconnect) {
          connection->state = ConnectionState::reconnecting;
          reconnect = true;
        } else return;
      }
      DriverError error;
      if (reconnect) {
        try { sessions = factory(config, error); }
        catch (const std::exception& exception) { error = {-4, exception.what()}; }
        catch (...) { error = {-4, "internal reconnect failure"}; }
        if (sessions) {
          {
            std::lock_guard lock(connection->mutex);
            if (connection->state != ConnectionState::reconnecting) return;
            connection->sessions = sessions;
            connection->escape_context.store(make_escape_snapshot(pool, config.backend, sessions),
                                             std::memory_order_release);
            connection->state = ConnectionState::connected;
          }
          schedule_drain(pool, connection);
          schedule_health(pool, connection, factory, context, handle, amx,
                          std::chrono::seconds(5), std::chrono::seconds(1));
          return;
        }
        {
          std::lock_guard lock(connection->mutex);
          if (connection->state != ConnectionState::reconnecting) return;
          connection->state = ConnectionState::disconnected;
        }
        const auto next = next_reconnect_delay(retry);
        schedule_health(pool, connection, std::move(factory), context, handle, amx, retry, next);
        return;
      }
      bool healthy = false;
      try { healthy = sessions && sessions->ping(error); }
      catch (...) {}
      if (healthy) {
        schedule_drain(pool, connection);
        schedule_health(pool, connection, std::move(factory), context, handle, amx,
                        std::chrono::seconds(5), std::chrono::seconds(1));
        return;
      }
      {
        std::lock_guard lock(connection->mutex);
        if (connection->state != ConnectionState::connected) return;
        connection->sessions.reset();
        connection->state = ConnectionState::disconnected;
        if (!config.auto_reconnect) return;
      }
      schedule_health(pool, connection, std::move(factory), context, handle, amx,
                      std::chrono::seconds(1), std::chrono::seconds(2));
    });
  }

  static std::shared_ptr<const EscapeSnapshot> make_escape_snapshot(
      WorkerPool* pool, Backend backend, const std::shared_ptr<SessionPool>& sessions) {
    auto* snapshot = new EscapeSnapshot{backend, sessions};
    return {snapshot, [pool](const EscapeSnapshot* old) {
      pool->defer_cleanup(0, [old] { delete old; });
    }};
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
