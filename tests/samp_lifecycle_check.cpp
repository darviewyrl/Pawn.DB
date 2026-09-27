#include <windows.h>
#include <malloc.h>
#include <plugincommon.h>
#include <amx/amx.h>

#include <array>

namespace {

int registrations = 0;

int AMXAPI register_empty(AMX*, const AMX_NATIVE_INFO* natives, int count) {
  if (count != 0 || natives[0].name || natives[0].func) return AMX_ERR_PARAMS;
  ++registrations;
  return AMX_ERR_NONE;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  HMODULE library = LoadLibraryA(argv[1]);
  if (!library) return 1;
  auto supports = reinterpret_cast<unsigned int(PLUGIN_CALL*)()>(GetProcAddress(library, "Supports"));
  auto load = reinterpret_cast<bool(PLUGIN_CALL*)(void**)>(GetProcAddress(library, "Load"));
  auto unload = reinterpret_cast<void(PLUGIN_CALL*)()>(GetProcAddress(library, "Unload"));
  auto tick = reinterpret_cast<void(PLUGIN_CALL*)()>(GetProcAddress(library, "ProcessTick"));
  auto amx_load = reinterpret_cast<int(PLUGIN_CALL*)(AMX*)>(GetProcAddress(library, "AmxLoad"));
  auto amx_unload = reinterpret_cast<int(PLUGIN_CALL*)(AMX*)>(GetProcAddress(library, "AmxUnload"));
  if (!supports || !load || !unload || !tick || !amx_load || !amx_unload) return 1;
  if (supports() != (SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK)) return 1;
  if (load(nullptr)) return 1;
  std::array<void*, PLUGIN_AMX_EXPORT_Register + 1> exports{};
  exports[PLUGIN_AMX_EXPORT_Register] = reinterpret_cast<void*>(&register_empty);
  std::array<void*, PLUGIN_DATA_AMX_EXPORTS + 1> data{};
  data[PLUGIN_DATA_AMX_EXPORTS] = exports.data();
  if (!load(data.data())) return 1;
  AMX amx{};
  if (amx_load(&amx) != AMX_ERR_NONE || registrations != 1) return 1;
  tick();
  if (amx_unload(&amx) != AMX_ERR_NONE) return 1;
  unload();
  const bool ok = amx_load(&amx) == AMX_ERR_PARAMS && registrations == 1;
  FreeLibrary(library);
  return ok ? 0 : 1;
}
