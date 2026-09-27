#include <windows.h>
#include <malloc.h>
#include <plugincommon.h>
#include <amx/amx.h>

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

int registrations = 0;
AMX_NATIVE connect_file_native = nullptr;
cell expected_handle = 0;
int callbacks = 0;
std::string message;
std::string warning;
std::vector<cell> arguments;
std::array<cell, 64> path_cells{};

void fake_logprintf(char* format, ...) {
  char output[512]{};
  va_list args;
  va_start(args, format);
  std::vsnprintf(output, sizeof(output), format, args);
  va_end(args);
  warning = output;
}

int AMXAPI fake_get_addr(AMX*, cell address, cell** out) {
  if (address != 1) return AMX_ERR_PARAMS;
  *out = path_cells.data();
  return AMX_ERR_NONE;
}
int AMXAPI fake_str_len(const cell* source, int* length) {
  *length = 0;
  while (source[*length]) ++*length;
  return AMX_ERR_NONE;
}
int AMXAPI fake_get_string(char* output, const cell* source, int, size_t size) {
  size_t i = 0;
  while (i + 1 < size && source[i]) { output[i] = static_cast<char>(source[i]); ++i; }
  output[i] = '\0';
  return AMX_ERR_NONE;
}
int AMXAPI fake_find_public(AMX*, const char* name, int* index) {
  if (std::strcmp(name, "OnConnectionError")) return AMX_ERR_NOTFOUND;
  *index = 1;
  return AMX_ERR_NONE;
}
int AMXAPI fake_push_string(AMX*, cell* address, cell**, const char* text, int, int) {
  message = text;
  *address = 99;
  return AMX_ERR_NONE;
}
int AMXAPI fake_push(AMX*, cell value) { arguments.push_back(value); return AMX_ERR_NONE; }
int AMXAPI fake_exec(AMX*, cell*, int index) {
  if (index == 1 && arguments == std::vector<cell>{-1, expected_handle} && !message.empty())
    ++callbacks;
  arguments.clear();
  return AMX_ERR_NONE;
}
int AMXAPI fake_release(AMX*, cell) { return AMX_ERR_NONE; }

int AMXAPI register_connections(AMX* amx, const AMX_NATIVE_INFO* natives, int count) {
  if (count != 16 || natives[count].name || natives[count].func) return AMX_ERR_PARAMS;
  for (int i = 0; i < count; ++i)
    if (!natives[i].name || std::strncmp(natives[i].name, "pdb_", 4) || !natives[i].func)
      return AMX_ERR_PARAMS;
  cell params[] = {sizeof(cell), 0};
  if (natives[3].func(amx, params) != 0) return AMX_ERR_PARAMS;
  connect_file_native = natives[1].func;
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
  std::array<void*, PLUGIN_AMX_EXPORT_StrLen + 1> exports{};
  exports[PLUGIN_AMX_EXPORT_Register] = reinterpret_cast<void*>(&register_connections);
  exports[PLUGIN_AMX_EXPORT_GetAddr] = reinterpret_cast<void*>(&fake_get_addr);
  exports[PLUGIN_AMX_EXPORT_StrLen] = reinterpret_cast<void*>(&fake_str_len);
  exports[PLUGIN_AMX_EXPORT_GetString] = reinterpret_cast<void*>(&fake_get_string);
  exports[PLUGIN_AMX_EXPORT_FindPublic] = reinterpret_cast<void*>(&fake_find_public);
  exports[PLUGIN_AMX_EXPORT_PushString] = reinterpret_cast<void*>(&fake_push_string);
  exports[PLUGIN_AMX_EXPORT_Push] = reinterpret_cast<void*>(&fake_push);
  exports[PLUGIN_AMX_EXPORT_Exec] = reinterpret_cast<void*>(&fake_exec);
  exports[PLUGIN_AMX_EXPORT_Release] = reinterpret_cast<void*>(&fake_release);
  std::array<void*, PLUGIN_DATA_AMX_EXPORTS + 1> data{};
  data[PLUGIN_DATA_LOGPRINTF] = reinterpret_cast<void*>(&fake_logprintf);
  data[PLUGIN_DATA_AMX_EXPORTS] = exports.data();
  if (!load(data.data())) return 1;
  AMX amx{};
  if (amx_load(&amx) != AMX_ERR_NONE || registrations != 1) return 1;
  const std::filesystem::path missing = "pdb_samp_missing.json";
  std::filesystem::remove(missing);
  const auto path = missing.string();
  for (std::size_t i = 0; i < path.size(); ++i) path_cells[i] = path[i];
  cell connect_params[] = {sizeof(cell), 1};
  expected_handle = connect_file_native(&amx, connect_params);
  if (!expected_handle) return 1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!callbacks && std::chrono::steady_clock::now() < deadline) {
    tick();
    std::this_thread::yield();
  }
  if (callbacks != 1 || !std::filesystem::exists(missing) ||
      warning.find("A default template has been generated") == std::string::npos) return 1;
  std::filesystem::remove(missing);
  tick();
  if (amx_unload(&amx) != AMX_ERR_NONE) return 1;
  unload();
  const bool ok = amx_load(&amx) == AMX_ERR_PARAMS && registrations == 1;
  FreeLibrary(library);
  return ok ? 0 : 1;
}
