#include <pawndb/handle_registry.hpp>

#include <memory>

int main() {
  unsigned warnings = 0;
  pawndb::HandleRegistry registry([&](auto) { ++warnings; });
  auto first = registry.insert(std::make_shared<int>(7));
  if (!first || *registry.get<int>(first) != 7) return 1;
  if (registry.get<float>(first)) return 1;
  if (registry.erase<float>(first)) return 1;
  auto borrowed = registry.get<int>(first);
  if (!registry.erase<int>(first) || *borrowed != 7) return 1;
  if (registry.get<int>(first) || registry.erase<int>(first)) return 1;
  auto second = registry.insert(std::make_shared<int>(9));
  if (second == first || *registry.get<int>(second) != 9) return 1;
  pawndb::HandleRegistry namespaced;
  auto handle = namespaced.insert(std::make_shared<int>(1), true);
  const auto stale = handle;
  for (int i = 0; i < 2048; ++i) {
    if (!(handle & pawndb::HandleRegistry::kAddressExcluded) || !namespaced.erase<int>(handle)) return 1;
    handle = namespaced.insert(std::make_shared<int>(1), true);
    if (namespaced.get<int>(stale, false)) return 1;
  }
  if ((stale & 0xfffff) == (handle & 0xfffff)) return 1;
  return !registry.get<int>(0) && warnings == 5 ? 0 : 1;
}
