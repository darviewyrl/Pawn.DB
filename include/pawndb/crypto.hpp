#pragma once

#include <pawndb/lifecycle.hpp>
#include <argon2.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <array>
#include <charconv>
#include <string>
#include <string_view>

namespace pawndb {

inline std::string hash_password(std::string_view password) {
  std::array<unsigned char, 16> salt{};
  std::array<char, 128> encoded{};
  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
      argon2id_hash_encoded(3, 65536, 4, password.data(), password.size(),
          salt.data(), salt.size(), 32, encoded.data(), encoded.size()) != ARGON2_OK)
    return {};
  return encoded.data();
}

inline bool verify_password(std::string_view password, const std::string& encoded) {
  std::string_view parameters(encoded);
  if (encoded.size() > 512 || !parameters.starts_with("$argon2id$v=19$m=")) return false;
  parameters.remove_prefix(std::string_view("$argon2id$v=19$m=").size());
  auto consume = [&](unsigned& value, std::string_view delimiter) {
    const auto end = parameters.data() + parameters.size();
    const auto parsed = std::from_chars(parameters.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr == end) return false;
    parameters.remove_prefix(static_cast<std::size_t>(parsed.ptr - parameters.data()));
    if (!parameters.starts_with(delimiter)) return false;
    parameters.remove_prefix(delimiter.size());
    return true;
  };
  unsigned memory = 0, passes = 0, lanes = 0;
  if (!consume(memory, ",t=") || !consume(passes, ",p=") || !consume(lanes, "$") ||
      memory > 262144 || passes > 10 || lanes > 16) return false;
  // argon2id_verify uses argon2_compare: a full-length XOR comparison of hash bytes.
  return argon2id_verify(encoded.c_str(), password.data(), password.size()) == ARGON2_OK;
}

class CryptoEngine {
 public:
  using Completion = std::function<void(std::string, bool)>;
  explicit CryptoEngine(Lifecycle& life) : life_(life) {}

  bool submit(void* amx, std::string password, std::string encoded, bool verify,
              Completion completion) {
    const auto context = life_.context(amx);
    if (context.expired() || !completion || password.size() > 4096 || encoded.size() > 512)
      return false;
    auto* pool = life_.worker_pool();
    if (!pool) return false;
    return pool->submit(next_worker_++ % pool->size(), WorkerPool::Priority::normal,
        [pool, context, password = std::move(password), encoded = std::move(encoded),
         verify, completion = std::move(completion)]() mutable {
          std::string hash;
          bool success = false;
          try {
            if (verify) success = verify_password(password, encoded);
            else hash = hash_password(password);
          } catch (...) {}
          OPENSSL_cleanse(password.data(), password.size());
          try {
            pool->publish(Lifecycle::guard_callback(context,
                [completion = std::move(completion), hash = std::move(hash), success]() mutable {
                  completion(std::move(hash), success);
                }));
          } catch (...) {}
        });
  }

 private:
  Lifecycle& life_;
  std::size_t next_worker_ = 0;
};

}  // namespace pawndb
