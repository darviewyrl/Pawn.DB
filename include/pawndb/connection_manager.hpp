#pragma once

#include <pawndb/connection_config.hpp>
#include <pawndb/handle_registry.hpp>
#include <pawndb/lifecycle.hpp>
#include <pawndb/result.hpp>
#include <pawndb/session_pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
  WorkerPool::Priority priority;
  std::function<void(bool, DriverError, QueryResult)> completion;
  Lifecycle::Context context;
  void* amx;
  std::uint32_t handle;
  bool flush_on_shutdown;
  bool capture_result;
  std::vector<std::string> batch_statements;
  bool batch_atomic = false;
  std::function<void(BatchExecutionResult)> batch_completion;
};

struct ConnectionMetrics {
  std::uint64_t qps = 0;
  std::uint64_t pending = 0;
  std::uint64_t average_latency_us = 0;
  std::uint64_t slow_queries = 0;
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
  bool in_flight_flush = false;
  bool shutting_down = false;
  std::condition_variable drain_ready;
  std::mutex metrics_mutex;
  std::deque<std::chrono::steady_clock::time_point> successful_queries;
  std::atomic<std::uint64_t> completed_queries{0};
  std::atomic<std::uint64_t> total_latency_us{0};
  std::atomic<std::uint64_t> slow_query_threshold_us{0};
  std::atomic<std::uint64_t> slow_queries{0};

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

struct BatchConfiguration {
  BatchConfiguration(void* script, std::uint32_t connection_handle, bool transaction)
      : owner(script), connection(connection_handle), atomic(transaction) {}
  void* owner;
  HandleRegistry::Handle connection;
  bool atomic;
  std::vector<std::string> statements;
};

class ConnectionManager {
 public:
  using Handle = HandleRegistry::Handle;
  using QueryCompletion = std::function<void(bool, DriverError, QueryResult)>;
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
    if (!accepting_.load(std::memory_order_acquire)) return 0;
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
    if (!accepting_.load(std::memory_order_acquire) || !amx || path.empty()) return 0;
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
    connection->drain_ready.notify_all();
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

  bool query(Handle handle, std::string sql, QueryCompletion completion = {},
             WorkerPool::Priority priority = WorkerPool::Priority::normal,
             bool flush_on_shutdown = false, bool capture_result = false) {
    if (!accepting_.load(std::memory_order_acquire)) return false;
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
          {std::move(sql), priority, std::move(completion), life_.context(connection->owner),
           connection->owner, handle, flush_on_shutdown, capture_result, {}, false, {}});
      start_drain = state == ConnectionState::connected && !connection->draining_queries;
    }
    if (start_drain) schedule_drain(pool, connection);
    return true;
  }

  Handle create_batch(void* amx, Handle connection_handle, bool atomic) {
    auto connection = handles_.get<Connection>(connection_handle);
    if (!amx || !connection || connection->owner != amx ||
        connection->state == ConnectionState::closing || connection->state == ConnectionState::failed)
      return 0;
    const auto handle = handles_.insert(
        std::make_shared<BatchConfiguration>(amx, connection_handle, atomic));
    if (handle) owners_[amx].insert(handle);
    return handle;
  }

  bool add_batch(void* amx, Handle handle, std::string sql) {
    auto batch = handles_.get<BatchConfiguration>(handle);
    if (!batch || batch->owner != amx || sql.empty() || sql.size() > 65536 ||
        batch->statements.size() >= 8192) return false;
    batch->statements.push_back(std::move(sql));
    return true;
  }

  std::optional<Handle> batch_connection(void* amx, Handle handle) {
    auto batch = handles_.get<BatchConfiguration>(handle);
    return batch && batch->owner == amx ? std::optional<Handle>(batch->connection) : std::nullopt;
  }

  bool free_batch(void* amx, Handle handle) {
    auto batch = handles_.get<BatchConfiguration>(handle);
    if (!batch || batch->owner != amx || !handles_.erase<BatchConfiguration>(handle)) return false;
    if (auto owner = owners_.find(amx); owner != owners_.end()) owner->second.erase(handle);
    return true;
  }

  using BatchCompletion = std::function<void(BatchExecutionResult)>;
  bool submit_batch(void* amx, Handle handle, BatchCompletion completion) {
    if (!accepting_.load(std::memory_order_acquire) || !amx || !completion) return false;
    auto batch = handles_.get<BatchConfiguration>(handle);
    if (!batch || batch->owner != amx || batch->statements.empty()) return false;
    auto connection = handles_.get<Connection>(batch->connection);
    if (!connection || connection->owner != amx) return false;
    auto* pool = life_.active_worker_pool();
    if (!pool) return false;
    PendingQuery pending{};
    pending.priority = WorkerPool::Priority::normal;
    pending.context = life_.context(amx);
    pending.amx = amx;
    pending.handle = batch->connection;
    pending.batch_statements = batch->statements;
    pending.batch_atomic = batch->atomic;
    pending.batch_completion = std::move(completion);
    bool start_drain = false;
    {
      std::lock_guard lock(connection->mutex);
      const auto state = connection->state.load();
      if (state == ConnectionState::closing || state == ConnectionState::failed ||
          connection->pending_queries.size() >= 8192 ||
          (state != ConnectionState::connected && connection->config &&
           !connection->config->auto_reconnect)) return false;
      connection->pending_queries.push_back(std::move(pending));
      start_drain = state == ConnectionState::connected && !connection->draining_queries;
    }
    free_batch(amx, handle);
    if (start_drain) schedule_drain(pool, connection);
    return true;
  }

  std::size_t shutdown(std::size_t max_flush = 500) {
    if (!accepting_.exchange(false, std::memory_order_acq_rel)) return 0;
    std::vector<std::shared_ptr<Connection>> connections;
    for (const auto& [_, owned] : owners_)
      for (const auto handle : owned)
        if (auto connection = handles_.get<Connection>(handle, false))
          connections.push_back(std::move(connection));

    auto remaining = max_flush;
    auto* pool = life_.active_worker_pool();
    std::size_t selected = 0;
    for (const auto& connection : connections) {
      bool start_drain = false;
      {
        std::lock_guard lock(connection->mutex);
        connection->shutting_down = true;
        if (connection->state == ConnectionState::pending) {
          connection->state = ConnectionState::closing;
          connection->pending_queries.clear();
          connection->drain_ready.notify_all();
          continue;
        }
        std::deque<PendingQuery> retained;
        if (connection->state == ConnectionState::connected) {
          if (connection->in_flight_flush && remaining) { --remaining; ++selected; }
          for (auto& query : connection->pending_queries) {
            if (query.flush_on_shutdown && remaining) {
              retained.push_back(std::move(query));
              --remaining;
              ++selected;
            }
          }
        }
        connection->pending_queries = std::move(retained);
        start_drain = connection->state == ConnectionState::connected &&
                      !connection->pending_queries.empty() && !connection->draining_queries;
      }
      if (start_drain && pool) schedule_drain(pool, connection);
    }
    for (const auto& connection : connections) {
      std::unique_lock lock(connection->mutex);
      if (!connection->pending_queries.empty() || connection->in_flight_flush)
        connection->drain_ready.wait(lock, [&] {
          return connection->state != ConnectionState::connected ||
                 (connection->pending_queries.empty() && !connection->draining_queries);
        });
    }
    return selected;
  }

  bool is_connected(Handle handle) const {
    auto connection = handles_.get<Connection>(handle);
    return connection && connection->state == ConnectionState::connected;
  }

  std::optional<ConnectionMetrics> metrics(Handle handle) const {
    auto connection = handles_.get<Connection>(handle);
    if (!connection) return std::nullopt;
    ConnectionMetrics result;
    {
      std::lock_guard lock(connection->mutex);
      result.pending = connection->pending_queries.size();
    }
    {
      std::lock_guard lock(connection->metrics_mutex);
      const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(1);
      while (!connection->successful_queries.empty() &&
             connection->successful_queries.front() < cutoff)
        connection->successful_queries.pop_front();
      result.qps = connection->successful_queries.size();
    }
    const auto completed = connection->completed_queries.load(std::memory_order_relaxed);
    if (completed)
      result.average_latency_us = connection->total_latency_us.load(std::memory_order_relaxed) /
                                  completed;
    result.slow_queries = connection->slow_queries.load(std::memory_order_relaxed);
    return result;
  }

  bool set_slow_query_threshold(Handle handle, std::uint64_t threshold_us) {
    auto connection = handles_.get<Connection>(handle);
    if (!connection) return false;
    connection->slow_query_threshold_us.store(threshold_us, std::memory_order_relaxed);
    return true;
  }

  Handle create_result(void* amx, QueryResult data) {
    if (life_.context(amx).expired()) return 0;
    Handle handle = 0;
    try {
      handle = handles_.insert(std::make_shared<ResultObject>(std::move(data), amx));
      if (handle) owners_[amx].insert(handle);
      return handle;
    } catch (...) {
      if (handle) handles_.erase<ResultObject>(handle);
      return 0;
    }
  }

  Handle create_batch_result(void* amx, BatchExecutionResult data) {
    if (life_.context(amx).expired()) return 0;
    std::vector<Handle> result_handles;
    Handle batch_handle = 0;
    std::shared_ptr<BatchResultObject> batch;
    try {
      batch = std::make_shared<BatchResultObject>(amx);
      result_handles.reserve(data.items.size());
      batch->items.reserve(data.items.size());
      for (auto& item : data.items) {
        BatchResultEntry entry;
        entry.status = item.status;
        entry.error_code = item.error.code;
        entry.error_message = std::move(item.error.message);
        if (item.status == BatchItemStatus::success) {
          entry.result_handle = create_result(amx, std::move(item.result));
          if (!entry.result_handle) throw std::bad_alloc();
          result_handles.push_back(entry.result_handle);
        }
        batch->items.push_back(std::move(entry));
      }
      batch_handle = handles_.insert(batch);
      if (!batch_handle) throw std::bad_alloc();
      owners_[amx].insert(batch_handle);
      return batch_handle;
    } catch (...) {
      if (batch_handle) {
        handles_.erase<BatchResultObject>(batch_handle);
        if (auto owner = owners_.find(amx); owner != owners_.end()) owner->second.erase(batch_handle);
      }
      for (const auto result : result_handles) erase_result(amx, result);
      return 0;
    }
  }

  std::shared_ptr<BatchResultObject> batch_result(void* amx, Handle handle) {
    auto value = handles_.get<BatchResultObject>(handle, false);
    if (!value || value->owner != amx) {
      warnings_("[Pawn.DB Warning] Attempted to access an invalid or deallocated PDBBatchResult handle.");
      return nullptr;
    }
    return value;
  }

  bool retain_batch_result(void* amx, Handle handle) {
    auto result = batch_result(amx, handle);
    if (!result) return false;
    result->retained = true;
    return true;
  }

  bool free_batch_result(void* amx, Handle handle) {
    auto result = batch_result(amx, handle);
    return result && erase_batch_result(amx, handle, result);
  }

  void release_scoped_batch_result(void* amx, Handle handle) {
    auto result = handles_.get<BatchResultObject>(handle, false);
    if (result && result->owner == amx && !result->retained)
      erase_batch_result(amx, handle, result);
  }

  bool retain_result(void* amx, Handle handle) {
    auto result = get_result(handle);
    if (!result || result->owner != amx) return false;
    result->retained = true;
    return true;
  }

  std::shared_ptr<ResultObject> result(void* amx, Handle handle) {
    auto value = get_result(handle);
    return value && value->owner == amx ? value : nullptr;
  }

  bool free_result(void* amx, Handle handle) {
    auto result = get_result(handle);
    return result && result->owner == amx && erase_result(amx, handle);
  }

  void release_scoped_result(void* amx, Handle handle) {
    auto result = handles_.get<ResultObject>(handle, false);
    if (result && result->owner == amx && !result->retained) erase_result(amx, handle);
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
      if (!owners_[amx].contains(handle)) continue;
      if (handles_.get<SetupConfiguration>(handle, false)) free_setup(handle);
      else if (handles_.get<BatchConfiguration>(handle, false)) free_batch(amx, handle);
      else if (handles_.get<ResultObject>(handle, false)) erase_result(amx, handle);
      else if (auto result = handles_.get<BatchResultObject>(handle, false))
        erase_batch_result(amx, handle, result);
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
  std::atomic<bool> accepting_{true};

  std::shared_ptr<ResultObject> get_result(Handle handle) {
    auto result = handles_.get<ResultObject>(handle, false);
    if (!result)
      warnings_("[Pawn.DB Warning] Attempted to access an invalid or deallocated PDBResult handle (Handle: " +
                std::to_string(static_cast<std::int32_t>(handle)) + ").");
    return result;
  }

  bool erase_result(void* amx, Handle handle) {
    if (!handles_.erase<ResultObject>(handle)) return false;
    if (auto owner = owners_.find(amx); owner != owners_.end()) {
      owner->second.erase(handle);
    }
    return true;
  }

  bool erase_batch_result(void* amx, Handle handle,
                          const std::shared_ptr<BatchResultObject>& result) {
    for (const auto& item : result->items)
      if (item.result_handle) release_scoped_result(amx, item.result_handle);
    if (!handles_.erase<BatchResultObject>(handle)) return false;
    if (auto owner = owners_.find(amx); owner != owners_.end()) owner->second.erase(handle);
    return true;
  }

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
    WorkerPool::Priority priority = WorkerPool::Priority::normal;
    bool start = false;
    {
      std::lock_guard lock(connection->mutex);
      if (connection->state == ConnectionState::connected && !connection->pending_queries.empty() &&
          !connection->draining_queries) {
        connection->draining_queries = true;
        if (std::any_of(connection->pending_queries.begin(), connection->pending_queries.end(),
                        [](const PendingQuery& query) {
                          return query.priority == WorkerPool::Priority::high;
                        })) priority = WorkerPool::Priority::high;
        start = true;
      }
    }
    if (start && !pool->submit(0, priority,
                               [pool, connection] { drain_one(pool, connection); })) {
      std::lock_guard lock(connection->mutex);
      connection->draining_queries = false;
      connection->drain_ready.notify_all();
    }
  }

  static void complete_query(WorkerPool* pool, PendingQuery query, bool success,
                             DriverError error = {}, QueryResult result = {}) {
    if (!query.completion) return;
    pool->publish(Lifecycle::guard_callback(
        query.context, [completion = std::move(query.completion), success,
        error = std::move(error), result = std::move(result)]() mutable {
          completion(success, std::move(error), std::move(result));
        }));
  }

  static void complete_batch(WorkerPool* pool, PendingQuery query,
                             BatchExecutionResult result) {
    if (!query.batch_completion) return;
    pool->publish(Lifecycle::guard_callback(
        query.context, [completion = std::move(query.batch_completion),
                        result = std::move(result)]() mutable {
          completion(std::move(result));
        }));
  }

  static void drain_one(WorkerPool* pool, const std::shared_ptr<Connection>& connection) {
    PendingQuery query;
    std::shared_ptr<SessionPool> sessions;
    {
      std::lock_guard lock(connection->mutex);
      if (connection->state != ConnectionState::connected || connection->pending_queries.empty()) {
        connection->draining_queries = false;
        connection->drain_ready.notify_all();
        return;
      }
      auto it = std::find_if(connection->pending_queries.begin(), connection->pending_queries.end(),
                             [](const PendingQuery& pending) {
                               return pending.priority == WorkerPool::Priority::high;
                             });
      if (it == connection->pending_queries.end()) it = connection->pending_queries.begin();
      query = std::move(*it);
      connection->pending_queries.erase(it);
      connection->in_flight_flush = query.flush_on_shutdown;
      sessions = connection->sessions;
    }
    DriverError error;
    bool ok = false;
    QueryResult result;
    BatchExecutionResult batch_result;
    const bool is_batch = static_cast<bool>(query.batch_completion);
    const auto started = std::chrono::steady_clock::now();
    try {
      if (is_batch) {
        batch_result = sessions ? sessions->execute_batch(query.batch_statements,
                                                           query.batch_atomic) :
                                  BatchExecutionResult{};
        ok = std::all_of(batch_result.items.begin(), batch_result.items.end(),
            [](const BatchStatementResult& item) { return item.status == BatchItemStatus::success; });
      } else {
        ok = sessions && (query.capture_result ? sessions->query_result(query.sql, error, result)
                                               : sessions->query(query.sql, error));
      }
    }
    catch (const std::exception& exception) { error = {-4, exception.what()}; }
    catch (...) { error = {-4, "internal query failure"}; }
    const auto finished = std::chrono::steady_clock::now();
    const auto latency_us = static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::microseconds>(finished - started).count());
    connection->completed_queries.fetch_add(1, std::memory_order_relaxed);
    connection->total_latency_us.fetch_add(latency_us, std::memory_order_relaxed);
    const auto threshold = connection->slow_query_threshold_us.load(std::memory_order_relaxed);
    if (threshold && latency_us >= threshold)
      connection->slow_queries.fetch_add(1, std::memory_order_relaxed);
    if (ok) {
      std::lock_guard lock(connection->metrics_mutex);
      connection->successful_queries.push_back(finished);
      const auto cutoff = finished - std::chrono::seconds(1);
      while (connection->successful_queries.front() < cutoff)
        connection->successful_queries.pop_front();
    }
    const bool connection_failed = is_batch ? batch_result.connection_error : !ok;
    if (connection_failed) {
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
        connection->drain_ready.notify_all();
      }
      if (is_batch) complete_batch(pool, std::move(query), std::move(batch_result));
      else complete_query(pool, std::move(query), false, std::move(error));
    } else {
      if (is_batch) complete_batch(pool, std::move(query), std::move(batch_result));
      else complete_query(pool, std::move(query), true, {}, std::move(result));
    }
    {
      std::lock_guard lock(connection->mutex);
      connection->in_flight_flush = false;
      connection->draining_queries = false;
      connection->drain_ready.notify_all();
    }
    schedule_drain(pool, connection);
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
            connection->state == ConnectionState::failed || connection->shutting_down ||
            !connection->config) return;
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
