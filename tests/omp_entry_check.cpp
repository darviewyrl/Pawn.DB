#include <sdk.hpp>
#include <windows.h>

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  HMODULE library = LoadLibraryA(argv[1]);
  if (!library) return 1;
  auto entry = reinterpret_cast<ComponentEntryPoint_t>(GetProcAddress(library, "ComponentEntryPoint"));
  if (!entry) return 1;
  IComponent* component = entry();
  if (!component) return 1;
  const auto version = component->componentVersion();
  const bool ok = component->getUID() == UID(0x5d2ba4ab7ae534f9) &&
                  component->componentName() == "Pawn.DB" && version.major == 0 &&
                  version.minor == 0 && version.patch == 0 && version.prerel == 0;
  component->free();
  FreeLibrary(library);
  return ok ? 0 : 1;
}
