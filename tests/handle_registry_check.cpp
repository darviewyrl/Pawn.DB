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
  return !registry.get<int>(0) && warnings == 5 ? 0 : 1;
}
