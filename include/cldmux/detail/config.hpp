#pragma once

// Common dependencies, version macros, and low-level configuration shared by
// every private source fragment.

#define CLDMUX_VERSION "0.5.0"
#define CLDMUX_VERSION_NUM 0x000500

// Dependencies and compile-time contract --------------------------------------

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>


namespace cldmux {
namespace detail {
struct client_state;
}
} // namespace cldmux
namespace cldmux::gcp {

inline constexpr std::string_view version = CLDMUX_VERSION;

// Low-level GCP configuration and public values -------------------------------

// Every provider-facing failure uses the same exception type. HTTP failures
// retain the status and a compact response body for programmatic diagnostics;
// validation and transport failures leave http_status() as zero.
class Error : public std::runtime_error {
public:
    explicit Error(std::string message, long http_status = 0, std::string response = {})
        : std::runtime_error(std::move(message)), http_status_(http_status),
          response_(std::move(response)) {}

    [[nodiscard]] long http_status() const noexcept { return http_status_; }
    [[nodiscard]] const std::string& response() const noexcept { return response_; }

private:
    long http_status_ = 0;
    std::string response_;
};

// Token callbacks may provide an expiry and a quota/billing project. A missing
// expiry means the token is treated as non-expiring; a zero time point receives
// a conservative 50-minute lifetime when cached.
struct AccessToken {
    std::string value;
    std::chrono::system_clock::time_point expires_at = std::chrono::system_clock::time_point::max();
    std::string quota_project;
};

namespace detail {
AccessToken automatic_token();
AccessToken metadata_token();
} // namespace detail

// Copyable, lazy GCP bearer credentials. Copies share a mutex-protected token
// cache, which lets clients and handles refresh one callback result together.
class Credentials {
public:
    using Provider = std::function<AccessToken()>;

    Credentials() : Credentials(automatic()) {}

    static Credentials automatic() {
        return from([] { return detail::automatic_token(); });
    }
    static Credentials metadata() {
        return from([] { return detail::metadata_token(); });
    }

    static Credentials bearer(std::string token) {
        if (token.empty())
            throw Error("Bearer token must not be empty");
        return from([token = std::move(token)] {
            return AccessToken{token, std::chrono::system_clock::time_point::max(), {}};
        });
    }

    static Credentials from(Provider provider) {
        if (!provider)
            throw Error("Credential provider must not be empty");
        return Credentials(std::make_shared<State>(std::move(provider)));
    }

    // Copies share one cache. Tokens refresh five minutes early so a request
    // does not start with a credential likely to expire in flight.
    [[nodiscard]] AccessToken access_token() const {
        std::lock_guard lock(state_->mutex);
        const auto now = std::chrono::system_clock::now();
        if (state_->cached.value.empty() ||
            state_->cached.expires_at <= now + std::chrono::minutes(5)) {
            auto fresh = state_->provider();
            if (fresh.value.empty())
                throw Error("Credential provider returned an empty token");
            if (fresh.expires_at == std::chrono::system_clock::time_point{}) {
                fresh.expires_at = now + std::chrono::minutes(50);
            }
            state_->cached = std::move(fresh);
        }
        return state_->cached;
    }

    void invalidate() const {
        std::lock_guard lock(state_->mutex);
        state_->cached = {};
    }

private:
    struct State {
        explicit State(Provider p) : provider(std::move(p)) {}
        Provider provider;
        AccessToken cached;
        std::mutex mutex;
    };

    explicit Credentials(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

// Configuration for the low-level cldmux::gcp API. The portable cldmux::config
// below is normally preferable for jobs spanning more than one provider.
struct Config {
    // Empty values are discovered from GOOGLE_CLOUD_PROJECT / GOOGLE_CLOUD_ZONE,
    // then the GCP metadata server.
    std::string project;
    std::string zone;
    Credentials credentials;

    // Explicit value wins; GOOGLE_CLOUD_QUOTA_PROJECT is the fallback. Leave
    // empty for resource-based quota.
    std::string quota_project;
    std::chrono::milliseconds timeout{60'000};
    std::chrono::milliseconds transfer_timeout{std::chrono::hours(1)};

    // Endpoint overrides are handy for emulators, private proxies, and tests.
    // Plain HTTP is rejected unless this is set explicitly.
    bool allow_insecure_http = false;
    std::string storage_endpoint = "https://storage.googleapis.com";
    std::string compute_endpoint = "https://compute.googleapis.com";
    std::string batch_endpoint = "https://batch.googleapis.com";
    std::string logging_endpoint = "https://logging.googleapis.com";
};

// Cloud Storage metadata returned by stat/list/upload operations. Times and
// hashes retain the spelling returned by the JSON API.
struct Object {
    std::string name;
    std::string generation;
    std::uint64_t size = 0;
    std::string content_type;
    std::string updated;
    std::string etag;
    std::string crc32c;
    std::string md5_hash;
};

// A delimiter-based listing may contain both concrete objects and common
// prefixes. Pagination is hidden by Bucket::list().
struct ObjectList {
    std::vector<Object> objects;
    std::vector<std::string> prefixes;
};

struct ListOptions {
    std::string prefix;
    std::string delimiter;
    bool versions = false;
    // Zero means fetch every page. Otherwise stop after this many objects.
    std::size_t limit = 0;
};

struct PutOptions {
    std::string content_type = "application/octet-stream";
    // "0" means create-only. A known generation means compare-and-replace.
    std::optional<std::string> if_generation_match;
    bool crc32c = true;
};

// A compact view of one GCE instance; absent IPs remain empty strings.
struct Instance {
    std::string id;
    std::string name;
    std::string zone;
    std::string machine_type;
    std::string status;
    std::string internal_ip;
    std::string external_ip;
    std::string creation_timestamp;
};

class Cloud;
class Bucket;
class Vm;
class Operation;

} // namespace cldmux::gcp
