#include <pawndb/update_checker.hpp>

int main() {
  pawndb::SemanticVersion alpha{}, alpha_one{}, beta{}, stable{}, newer{}, numeric_two{}, numeric_ten{};
  if (!pawndb::parse_semantic_version("v1.2.3-alpha", alpha) ||
      !pawndb::parse_semantic_version("1.2.3-alpha.1", alpha_one) ||
      !pawndb::parse_semantic_version("1.2.3-beta+build.4", beta) ||
      !pawndb::parse_semantic_version("1.2.3", stable) ||
      !pawndb::parse_semantic_version("1.10.0", newer) ||
      !pawndb::parse_semantic_version("1.2.3-alpha.2", numeric_two) ||
      !pawndb::parse_semantic_version("1.2.3-alpha.10", numeric_ten) ||
      !pawndb::semantic_version_less(alpha, alpha_one) ||
      !pawndb::semantic_version_less(alpha_one, beta) ||
      !pawndb::semantic_version_less(beta, stable) ||
      !pawndb::semantic_version_less(stable, newer) ||
      !pawndb::semantic_version_less(numeric_two, numeric_ten) ||
      pawndb::kUpdateTimeoutMs != 3000) return 1;
  pawndb::SemanticVersion invalid{};
  return pawndb::parse_semantic_version("1.02.3", invalid) ||
                 pawndb::parse_semantic_version("1.2.3-alpha.", invalid) ||
                 pawndb::parse_semantic_version("1.2.3+build.", invalid) ||
                 pawndb::parse_semantic_version("1.2", invalid)
             ? 1 : 0;
}
