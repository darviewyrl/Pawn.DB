#include <pawndb/lifecycle.hpp>
#include <pawndb/connection_natives.hpp>
#include <pawndb/mariadb_pool.hpp>
#include <pawndb/postgres_pool.hpp>
#include <pawndb/update_checker.hpp>
#include <plugincommon.h>
#include <amx/amx.h>

#include <memory>
#include <string>

namespace {

pawndb::Lifecycle lifecycle;
pawndb::UpdateChecker update_checker;
decltype(&amx_Register) register_natives = nullptr;
decltype(&amx_GetAddr) get_addr = nullptr;
decltype(&amx_StrLen) str_len = nullptr;
decltype(&amx_GetString) get_string = nullptr;
decltype(&amx_SetString) set_string = nullptr;
decltype(&amx_FindPublic) find_public = nullptr;
decltype(&amx_PushString) push_string = nullptr;
decltype(&amx_Push) push = nullptr;
decltype(&amx_Release) release = nullptr;
decltype(&amx_Exec) exec = nullptr;
using LogPrintf = void (*)(char*, ...);
LogPrintf logprintf = nullptr;
std::unique_ptr<pawndb::ConnectionManager> connections;
std::unique_ptr<pawndb::ConnectionNatives> natives;

void connection_error(AMX* amx, pawndb::ConnectionManager::Handle handle,
                      int code, const std::string& message) {
  if (!find_public || !push_string || !push || !release || !exec) return;
  int index = 0;
  if (find_public(amx, "OnConnectionError", &index) != AMX_ERR_NONE) return;
  cell address = 0, result = 0;
  if (push_string(amx, &address, nullptr, message.c_str(), 0, 0) != AMX_ERR_NONE) return;
  if (push(amx, code) == AMX_ERR_NONE &&
      push(amx, static_cast<cell>(handle)) == AMX_ERR_NONE)
    exec(amx, &result, index);
  release(amx, address);
}

}  // namespace

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports() {
  return SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void** data) {
  if (!data || !data[PLUGIN_DATA_AMX_EXPORTS]) return false;
  auto* exports = static_cast<void**>(data[PLUGIN_DATA_AMX_EXPORTS]);
  register_natives = reinterpret_cast<decltype(&amx_Register)>(exports[PLUGIN_AMX_EXPORT_Register]);
  if (!register_natives) return false;
  get_addr = reinterpret_cast<decltype(&amx_GetAddr)>(exports[PLUGIN_AMX_EXPORT_GetAddr]);
  str_len = reinterpret_cast<decltype(&amx_StrLen)>(exports[PLUGIN_AMX_EXPORT_StrLen]);
  get_string = reinterpret_cast<decltype(&amx_GetString)>(exports[PLUGIN_AMX_EXPORT_GetString]);
  set_string = reinterpret_cast<decltype(&amx_SetString)>(exports[PLUGIN_AMX_EXPORT_SetString]);
  find_public = reinterpret_cast<decltype(&amx_FindPublic)>(exports[PLUGIN_AMX_EXPORT_FindPublic]);
  push_string = reinterpret_cast<decltype(&amx_PushString)>(exports[PLUGIN_AMX_EXPORT_PushString]);
  push = reinterpret_cast<decltype(&amx_Push)>(exports[PLUGIN_AMX_EXPORT_Push]);
  release = reinterpret_cast<decltype(&amx_Release)>(exports[PLUGIN_AMX_EXPORT_Release]);
  exec = reinterpret_cast<decltype(&amx_Exec)>(exports[PLUGIN_AMX_EXPORT_Exec]);
  logprintf = reinterpret_cast<LogPrintf>(data[PLUGIN_DATA_LOGPRINTF]);
  lifecycle.start();
  connections = std::make_unique<pawndb::ConnectionManager>(
      lifecycle, [](void* amx, auto handle, int code, std::string message) {
        connection_error(static_cast<AMX*>(amx), handle, code, message);
      }, [](std::string message) {
        if (logprintf) logprintf(const_cast<char*>("%s"), message.c_str());
      }, [](const pawndb::ConnectionConfig& config, pawndb::DriverError& error) {
        return config.backend == pawndb::Backend::postgres
                   ? pawndb::PostgresPool::open(config, error)
                   : pawndb::MariaPool::open(config, error);
      });
  natives = std::make_unique<pawndb::ConnectionNatives>(
      *connections,
      [](AMX* amx, cell address, std::string& value) {
        if (!get_addr || !str_len || !get_string) return false;
        cell* source = nullptr;
        int length = 0;
        if (get_addr(amx, address, &source) != AMX_ERR_NONE ||
            str_len(source, &length) != AMX_ERR_NONE || length < 0) return false;
        value.resize(static_cast<std::size_t>(length) + 1);
        if (get_string(value.data(), source, 0, value.size()) != AMX_ERR_NONE) return false;
        value.resize(length);
        return true;
      },
      [](AMX* amx, cell address, std::string_view value, std::size_t capacity) {
        if (!get_addr || !set_string || !capacity) return false;
        cell* output = nullptr;
        if (get_addr(amx, address, &output) != AMX_ERR_NONE) return false;
        const std::string truncated(value.substr(0, capacity - 1));
        return set_string(output, truncated.c_str(), 0, 0, capacity) == AMX_ERR_NONE;
      }, [] { return update_checker.available(); },
      [](AMX* amx, cell address, std::size_t& length) {
        if (!get_addr || !str_len) return false;
        cell* source = nullptr;
        int characters = 0;
        if (get_addr(amx, address, &source) != AMX_ERR_NONE ||
            str_len(source, &characters) != AMX_ERR_NONE || characters < 0) return false;
        length = static_cast<std::size_t>(characters);
        return true;
      },
      [](AMX* amx, cell address, std::span<char> output) {
        if (!get_addr || !get_string || output.empty()) return false;
        cell* source = nullptr;
        return get_addr(amx, address, &source) == AMX_ERR_NONE &&
               get_string(output.data(), source, 0, output.size()) == AMX_ERR_NONE;
      });
  return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload() {
  natives.reset();
  connections.reset();
  lifecycle.stop();
  update_checker.stop();
  register_natives = nullptr;
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick() { lifecycle.dispatch_tick(); }

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX* amx) {
  if (!amx || !register_natives) return AMX_ERR_PARAMS;
  const int result = register_natives(amx, pawndb::ConnectionNatives::table(),
                                      pawndb::ConnectionNatives::native_count);
  if (result == AMX_ERR_NONE && lifecycle.attach(amx)) {
    update_checker.start(lifecycle, [](std::string message) {
      if (logprintf) logprintf(const_cast<char*>("%s"), message.c_str());
    });
  }
  return result;
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX* amx) {
  if (connections) connections->detach(amx);
  lifecycle.detach(amx);
  return AMX_ERR_NONE;
}
