#include <argon2.h>
#include <curl/curl.h>
#include <libpq-fe.h>
#include <mysql.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>

int main() {
  const auto json = nlohmann::json::parse("{}");
  return json.is_object() && mysql_get_client_version() != 0 && PQlibVersion() != 0 &&
                 OpenSSL_version_num() != 0 && curl_version() != nullptr &&
                 argon2_type2string(Argon2_id, 0) != nullptr
             ? 0
             : 1;
}
