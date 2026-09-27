#pragma once

#include <pawndb/lifecycle.hpp>
#include <pawndb/version.hpp>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pawndb {

struct SemanticVersion {
  std::uint64_t major, minor, patch;
  std::vector<std::string> prerelease;
};

inline constexpr long kUpdateTimeoutMs = 3000;

inline bool semver_identifier_char(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') || c == '-';
}

inline bool parse_semantic_version(std::string_view value, SemanticVersion& output) {
  if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) value.remove_prefix(1);
  const auto build = value.find('+');
  if (build != std::string_view::npos) {
    auto metadata = value.substr(build + 1);
    while (!metadata.empty()) {
      const auto end = metadata.find('.');
      const auto id = metadata.substr(0, end);
      if (id.empty() || !std::all_of(id.begin(), id.end(), semver_identifier_char)) return false;
      if (end == std::string_view::npos) break;
      metadata.remove_prefix(end + 1);
    }
    if (metadata.empty()) return false;
    value = value.substr(0, build);
  }
  const auto dash = value.find('-');
  auto core = value.substr(0, dash);
  std::uint64_t* fields[] = {&output.major, &output.minor, &output.patch};
  for (int i = 0; i < 3; ++i) {
    const auto end = core.find('.', 0);
    const auto part = i == 2 ? core : core.substr(0, end);
    if (part.empty() || (part.size() > 1 && part.front() == '0')) return false;
    std::uint64_t number = 0;
    for (const char c : part) {
      if (c < '0' || c > '9' ||
          number > ((std::numeric_limits<std::uint64_t>::max)() - (c - '0')) / 10) return false;
      number = number * 10 + (c - '0');
    }
    *fields[i] = number;
    if (i < 2) {
      if (end == std::string_view::npos) return false;
      core.remove_prefix(end + 1);
    } else if (part.find('.') != std::string_view::npos) return false;
  }
  output.prerelease.clear();
  if (dash == std::string_view::npos) return true;
  auto prerelease = value.substr(dash + 1);
  while (!prerelease.empty()) {
    const auto end = prerelease.find('.');
    const auto id = prerelease.substr(0, end);
    if (id.empty()) return false;
    bool numeric = true;
    for (const unsigned char c : id) {
      if (!semver_identifier_char(c)) return false;
      numeric &= c >= '0' && c <= '9';
    }
    if (numeric && id.size() > 1 && id.front() == '0') return false;
    output.prerelease.emplace_back(id);
    if (end == std::string_view::npos) break;
    prerelease.remove_prefix(end + 1);
    if (prerelease.empty()) return false;
  }
  return true;
}

inline bool semantic_version_less(const SemanticVersion& left, const SemanticVersion& right) {
  if (left.major != right.major) return left.major < right.major;
  if (left.minor != right.minor) return left.minor < right.minor;
  if (left.patch != right.patch) return left.patch < right.patch;
  if (left.prerelease.empty() != right.prerelease.empty()) return !left.prerelease.empty();
  const auto count = left.prerelease.size() < right.prerelease.size()
                         ? left.prerelease.size() : right.prerelease.size();
  for (std::size_t i = 0; i < count; ++i) {
    const auto& a = left.prerelease[i];
    const auto& b = right.prerelease[i];
    const auto numeric = [](std::string_view id) {
      return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
      });
    };
    const bool an = numeric(a), bn = numeric(b);
    if (an != bn) return an;
    if (an && a.size() != b.size()) return a.size() < b.size();
    if (a != b) return a < b;
  }
  return left.prerelease.size() < right.prerelease.size();
}

class UpdateChecker {
 public:
  using Report = std::function<void(std::string)>;

  ~UpdateChecker() { stop(); }
  bool available() const noexcept { return available_.load(std::memory_order_acquire); }

  void start(Lifecycle& lifecycle, Report report) {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    available_.store(false, std::memory_order_release);
    auto* pool = lifecycle.worker_pool();
    if (!pool || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return;
    initialized_ = true;
    if (!pool->submit(pool->size() - 1, WorkerPool::Priority::normal,
                      [this, pool, report = std::move(report)] {
                        check(*pool, report);
                      })) stop();
  }

  void stop() {
    if (initialized_) {
      curl_global_cleanup();
      initialized_ = false;
    }
    started_.store(false, std::memory_order_release);
  }

 private:
  static std::size_t receive(char* data, std::size_t size, std::size_t count, void* context) {
    auto& body = *static_cast<std::string*>(context);
    const auto bytes = size * count;
    if (bytes > 1024 * 1024 - body.size()) return 0;
    body.append(data, bytes);
    return bytes;
  }

  void check(WorkerPool& pool, const Report& report) {
    std::string body;
    const std::string user_agent = "Pawn.DB/" + std::string(kVersion);
    CURL* curl = curl_easy_init();
    if (!curl) return;
    curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
    const bool configured =
        curl_easy_setopt(curl, CURLOPT_URL,
            "https://api.github.com/repos/darviewyrl/Pawn.DB/releases/latest") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kUpdateTimeoutMs) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kUpdateTimeoutMs) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body) == CURLE_OK;
    if (!configured) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return;
    }
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status != 200) return;
    try {
      const auto json = nlohmann::json::parse(body);
      const auto tag = json.at("tag_name").get<std::string>();
      const auto url = json.at("html_url").get<std::string>();
      SemanticVersion latest{}, current{};
      if (url.empty() || !parse_semantic_version(tag, latest) ||
          !parse_semantic_version(kVersion, current) || !semantic_version_less(current, latest))
        return;
      available_.store(true, std::memory_order_release);
      pool.publish([report, tag, url] {
        report("[Pawn.DB] A new version is available: " + tag + " (Current: v" +
               std::string(kVersion) + "). Download: " + url);
      });
    } catch (...) {}
  }

  std::atomic<bool> started_{false};
  std::atomic<bool> available_{false};
  bool initialized_ = false;
};

}  // namespace pawndb
