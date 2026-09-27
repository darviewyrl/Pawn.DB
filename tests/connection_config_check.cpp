#include <pawndb/connection_config.hpp>

#include <filesystem>
#include <fstream>

int main() {
  std::string error;
  const nlohmann::json required = {{"host", "localhost"}, {"user", "root"},
                                   {"password", ""}, {"database", "test"}};
  auto input = required;
  input["port"] = 5432;
  auto config = pawndb::parse_config(input, error);
  if (!config || config->backend != pawndb::Backend::postgres) return 1;
  input["driver"] = "mysql";
  config = pawndb::parse_config(input, error);
  if (!config || config->backend != pawndb::Backend::mariadb) return 1;
  input["port"] = 33306;
  config = pawndb::parse_config(input, error);
  if (!config || config->backend != pawndb::Backend::mariadb) return 1;
  input["ssl"] = {{"enable", true}, {"ca_cert", "ca.pem"},
                   {"client_cert", "client.pem"}, {"client_key", "client.key"}};
  config = pawndb::parse_config(input, error);
  if (!config || !config->ssl_enabled || !config->verify_server_cert ||
      config->client_cert != "client.pem") return 1;
  input["ssl"]["client_key"] = "";
  if (pawndb::parse_config(input, error) || error.empty()) return 1;
  input.erase("ssl");
  input.erase("port");
  input["driver"] = "postgres";
  config = pawndb::parse_config(input, error);
  if (!config || config->port != 5432) return 1;
  if (pawndb::parse_config({{"driver", "mysql"}}, error) || error.empty()) return 1;
  input["driver"] = "unknown";
  if (pawndb::parse_config(input, error) || error.empty()) return 1;
  const auto path = std::filesystem::current_path() / "pdb_test_config.json";
  std::filesystem::remove(path);
  if (pawndb::load_config_file(path, error) || !std::filesystem::exists(path)) return 1;
  config = pawndb::load_config_file(path, error);
  { std::ofstream invalid(path); invalid << "{bad"; }
  int code = 0;
  if (pawndb::load_config_file(path, error, &code) || code != -2) return 1;
  std::filesystem::remove(path);
  return config && config->backend == pawndb::Backend::mariadb ? 0 : 1;
}
