#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace pawndb {

enum class Backend { mariadb, postgres };
enum class DriverChoice { automatic, mariadb, postgres };

inline Backend infer_backend(int port) {
  return port == 5432 ? Backend::postgres : Backend::mariadb;
}

struct ConnectionConfig {
  Backend backend = Backend::mariadb;
  std::string host = "127.0.0.1";
  std::string user = "root";
  std::string password;
  std::string database = "server_db";
  std::string charset = "utf8mb4";
  int port = 3306;
  int pool_size = 4;
  int debug_level = 1;
  int connect_timeout = 5;
  bool auto_reconnect = true;
  bool multi_statements = false;
  bool ssl_enabled = false;
  bool verify_server_cert = true;
  std::string ca_cert = "certificates/ca.pem";
  std::string client_cert;
  std::string client_key;
};

inline nlohmann::json default_config_json() {
  return {{"driver", "mysql"}, {"host", "127.0.0.1"}, {"port", 3306},
          {"user", "root"}, {"password", ""}, {"database", "server_db"},
          {"charset", "utf8mb4"}, {"auto_reconnect", true},
          {"multi_statements", false}, {"pool_size", 4}, {"debug_level", 1},
          {"ssl", {{"enable", false}, {"ca_cert", "certificates/ca.pem"},
                   {"client_cert", ""}, {"client_key", ""}, {"verify_server_cert", true}}}};
}

inline std::optional<ConnectionConfig> parse_config(const nlohmann::json& json,
                                                     std::string& error) {
  try {
    if (!json.is_object()) throw std::invalid_argument("config must be a JSON object");
    for (const char* field : {"host", "user", "password", "database"})
      if (!json.contains(field) || !json.at(field).is_string())
        throw std::invalid_argument(std::string("missing or invalid field: ") + field);
    ConnectionConfig config;
    const auto driver = json.value("driver", std::string{});
    config.port = json.value("port", driver == "postgres" ? 5432 : 3306);
    if (driver == "postgres" || (driver.empty() && infer_backend(config.port) == Backend::postgres))
      config.backend = Backend::postgres;
    else if (!driver.empty() && driver != "mysql")
      throw std::invalid_argument("driver must be mysql or postgres");
    if (config.port < 1 || config.port > 65535)
      throw std::invalid_argument("port must be between 1 and 65535");
    config.host = json.value("host", config.host);
    config.user = json.value("user", config.user);
    config.password = json.value("password", config.password);
    config.database = json.value("database", config.database);
    config.charset = json.value("charset", config.charset);
    config.auto_reconnect = json.value("auto_reconnect", config.auto_reconnect);
    config.multi_statements = json.value("multi_statements", config.multi_statements);
    config.pool_size = json.value("pool_size", config.pool_size);
    config.debug_level = json.value("debug_level", config.debug_level);
    config.connect_timeout = json.value("connect_timeout", config.connect_timeout);
    if (config.pool_size < 1 || config.debug_level < 0 || config.debug_level > 3 ||
        config.connect_timeout < 1 ||
        config.host.empty() || config.user.empty() || config.database.empty() ||
        config.charset.empty())
      throw std::invalid_argument("invalid connection settings");
    if (json.contains("ssl")) {
      const auto& ssl = json.at("ssl");
      if (!ssl.is_object()) throw std::invalid_argument("ssl must be a JSON object");
      config.ssl_enabled = ssl.value("enable", config.ssl_enabled);
      config.ca_cert = ssl.value("ca_cert", config.ca_cert);
      config.client_cert = ssl.value("client_cert", config.client_cert);
      config.client_key = ssl.value("client_key", config.client_key);
      config.verify_server_cert = ssl.value("verify_server_cert", config.verify_server_cert);
    }
    if (config.ssl_enabled && (config.ca_cert.empty() ||
        config.client_cert.empty() != config.client_key.empty()))
      throw std::invalid_argument("invalid TLS certificate settings");
    return config;
  } catch (const std::exception& exception) {
    error = exception.what();
    return std::nullopt;
  }
}

inline std::optional<ConnectionConfig> load_config_file(const std::filesystem::path& path,
                                                         std::string& error, int* code = nullptr) {
  if (code) *code = -4;
  try {
    std::ifstream input(path);
    if (!input) {
      if (std::filesystem::exists(path)) throw std::runtime_error("cannot open config file");
      if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
      std::ofstream output(path);
      if (!output) throw std::runtime_error("cannot create config template");
      output << default_config_json().dump(2) << '\n';
      if (!output) throw std::runtime_error("cannot write config template");
      if (code) *code = -1;
      error = "configuration file was not found; a default template was generated";
      return std::nullopt;
    }
    auto config = parse_config(nlohmann::json::parse(input, nullptr, false), error);
    if (!config && code) *code = -2;
    return config;
  } catch (const std::exception& exception) {
    error = exception.what();
    return std::nullopt;
  }
}

}  // namespace pawndb
