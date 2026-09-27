#include <pawndb/lifecycle.hpp>
#include <plugincommon.h>
#include <amx/amx.h>

namespace {

pawndb::Lifecycle lifecycle;
decltype(&amx_Register) register_natives = nullptr;
constexpr AMX_NATIVE_INFO kNatives[] = {{nullptr, nullptr}};

}  // namespace

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports() {
  return SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void** data) {
  if (!data || !data[PLUGIN_DATA_AMX_EXPORTS]) return false;
  auto* exports = static_cast<void**>(data[PLUGIN_DATA_AMX_EXPORTS]);
  register_natives = reinterpret_cast<decltype(&amx_Register)>(exports[PLUGIN_AMX_EXPORT_Register]);
  if (!register_natives) return false;
  lifecycle.start();
  return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload() {
  lifecycle.stop();
  register_natives = nullptr;
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick() { lifecycle.dispatch_tick(); }

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX* amx) {
  if (!amx || !register_natives) return AMX_ERR_PARAMS;
  const int result = register_natives(amx, kNatives, 0);
  if (result == AMX_ERR_NONE) lifecycle.attach(amx);
  return result;
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX* amx) {
  lifecycle.detach(amx);
  return AMX_ERR_NONE;
}
