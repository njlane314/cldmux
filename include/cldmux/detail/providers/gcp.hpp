#pragma once

#include "cldmux/detail/http.hpp"

namespace cldmux::gcp {
namespace detail {

// GCP application default credentials -----------------------------------------

// Callers configure fixed bearer tokens or typed callbacks. For compatibility
// with gcloud, the automatic chain may also read its generated authorised-user
// ADC file before trying the metadata server; callers never have to author it.
inline bool metadata_port(std::string_view value) {
    if (value.empty())
        return false;
    unsigned port = 0;
    for (const char digit : value) {
        if (digit < '0' || digit > '9')
            return false;
        port = port * 10U + static_cast<unsigned>(digit - '0');
        if (port > 65'535U)
            return false;
    }
    return port != 0;
}

inline bool metadata_name(std::string_view value) {
    if (value.empty() || value.size() > 253 || value.front() == '.' || value.back() == '.')
        return false;
    bool dot = false;
    for (const char character : value) {
        const bool alpha_numeric =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9');
        if (!alpha_numeric && character != '-' && character != '_' && character != '.')
            return false;
        if (character == '.' && dot)
            return false;
        dot = character == '.';
    }
    return true;
}

inline bool metadata_authority(std::string_view value) {
    if (value.empty())
        return false;
    if (value.front() == '[') {
        const auto closing = value.find(']');
        if (closing == std::string_view::npos || closing == 1)
            return false;
        const auto address = value.substr(1, closing - 1);
        if (address.find(':') == std::string_view::npos ||
            !std::all_of(address.begin(), address.end(), [](const char character) {
                return (character >= '0' && character <= '9') ||
                       (character >= 'a' && character <= 'f') ||
                       (character >= 'A' && character <= 'F') || character == ':' ||
                       character == '.';
            }))
            return false;
        const auto suffix = value.substr(closing + 1);
        return suffix.empty() || (suffix.front() == ':' && metadata_port(suffix.substr(1)));
    }

    const auto colon = value.find(':');
    if (colon == std::string_view::npos)
        return metadata_name(value);
    return value.find(':', colon + 1) == std::string_view::npos &&
           metadata_name(value.substr(0, colon)) && metadata_port(value.substr(colon + 1));
}

inline std::string metadata_host() {
    std::string host = env("GCE_METADATA_HOST");
    if (host.empty())
        host = "metadata.google.internal";

    // GCE_METADATA_HOST is an authority override, not a URL. Bounding its
    // grammar prevents a launch-time setting from adding a scheme or path.
    if (!metadata_authority(host))
        throw Error("GCE_METADATA_HOST must contain only a host and optional port");

    // All GCE VM types support this host-local HTTP endpoint; HTTPS metadata
    // additionally requires Shielded-VM client certificates. no_proxy is set
    // on every request below. The process environment owner also controls the
    // earlier ADC and explicit bearer-token sources in this same chain.
    // codeql[cpp/non-https-url]
    return "http://" + host;
}

inline std::optional<std::string>
metadata_get(std::string_view path,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(800)) {
    try {
        auto response =
            http(HttpRequest{}
                     .with_method("GET")
                     .with_url(metadata_host() + "/computeMetadata/v1/" + std::string(path))
                     .with_headers({"Metadata-Flavor: Google"})
                     .with_no_proxy(true)
                     .with_timeout(timeout)
                     .with_connect_timeout(std::min(timeout, std::chrono::milliseconds(300))));
        if (response.status == 200)
            return trim(std::move(response.body));
    } catch (...) {
    }
    return std::nullopt;
}

inline AccessToken metadata_token() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        try {
            auto response =
                http(HttpRequest{}
                         .with_method("GET")
                         .with_url(metadata_host() +
                                   "/computeMetadata/v1/instance/service-accounts/default/token")
                         .with_headers({"Metadata-Flavor: Google"})
                         .with_no_proxy(true)
                         .with_timeout(std::chrono::milliseconds(1500))
                         .with_connect_timeout(std::chrono::milliseconds(300)));
            if (response.status == 200) {
                const Json json = parse_json(response.body);
                const std::string token = field(json, "access_token");
                const auto seconds = unsigned_field(json, "expires_in");
                if (token.empty() || seconds == 0)
                    throw Error("Malformed metadata token response");
                return AccessToken{
                    token, std::chrono::system_clock::now() + std::chrono::seconds(seconds), {}};
            }
            if (response.status != 429 && response.status < 500)
                throw Error("Metadata token request failed", response.status, response.body);
        } catch (const Error& error) {
            if (error.http_status() > 0 && error.http_status() != 429 && error.http_status() < 500)
                throw;
            if (attempt == 2)
                throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
    }
    throw Error("GCP metadata token is unavailable");
}

inline AccessToken refresh_authorised_user(const Json& json) {
    const std::string client_id = field(json, "client_id");
    const std::string client_secret = field(json, "client_secret");
    const std::string refresh_token = field(json, "refresh_token");
    if (client_id.empty() || client_secret.empty() || refresh_token.empty())
        throw Error("Malformed authorised-user ADC file");
    const std::string body = "grant_type=refresh_token&client_id=" + encode(client_id) +
                             "&client_secret=" + encode(client_secret) +
                             "&refresh_token=" + encode(refresh_token);
    auto response = http(HttpRequest{}
                             .with_method("POST")
                             .with_url("https://oauth2.googleapis.com/token")
                             .with_headers({"Content-Type: application/x-www-form-urlencoded"})
                             .with_body(body)
                             .with_timeout(std::chrono::milliseconds(30'000)));
    if (response.status < 200 || response.status >= 300)
        throw Error("OAuth token refresh failed", response.status, response.body);
    const Json token_json = parse_json(response.body);
    const std::string token = field(token_json, "access_token");
    const auto seconds = unsigned_field(token_json, "expires_in");
    if (token.empty() || seconds == 0)
        throw Error("Malformed OAuth token response");
    return AccessToken{token, std::chrono::system_clock::now() + std::chrono::seconds(seconds),
                       field(json, "quota_project_id")};
}

inline std::optional<std::filesystem::path> well_known_adc_path() {
    if (const std::string config = env("CLOUDSDK_CONFIG"); !config.empty())
        return std::filesystem::path(config) / "application_default_credentials.json";
#ifdef _WIN32
    if (const std::string appdata = env("APPDATA"); !appdata.empty())
        return std::filesystem::path(appdata) / "gcloud" / "application_default_credentials.json";
#else
    if (const std::string home = env("HOME"); !home.empty())
        return std::filesystem::path(home) / ".config" / "gcloud" /
               "application_default_credentials.json";
#endif
    return std::nullopt;
}

inline Json read_json_file(const std::filesystem::path& path,
                           std::size_t max_bytes = JsonLimits{}.max_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw Error("Cannot open credential file: " + path.string());
    std::string document;
    std::array<char, 8192> buffer{};
    while (true) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto bytes = static_cast<std::size_t>(count);
            if (bytes > max_bytes - std::min(document.size(), max_bytes))
                throw Error("Credential file exceeded the private byte limit: " + path.string());
            document.append(buffer.data(), bytes);
        }
        if (input.bad())
            throw Error("Cannot read credential file: " + path.string());
        if (input.eof())
            break;
        if (count == 0)
            throw Error("Cannot read credential file: " + path.string());
    }
    return parse_json(document);
}

inline AccessToken token_from_adc_file(const std::filesystem::path& path) {
    const Json json = read_json_file(path);
    const std::string type = field(json, "type");
    if (type == "authorized_user")
        return refresh_authorised_user(json);
    throw Error("Unsupported ADC credential type '" + type +
                "'; inject a token callback for this credential type");
}

inline AccessToken automatic_token() {
    if (std::string token = env("GCP_ACCESS_TOKEN"); !token.empty())
        return AccessToken{std::move(token), std::chrono::system_clock::time_point::max(), {}};
    if (std::string token = env("GOOGLE_OAUTH_ACCESS_TOKEN"); !token.empty())
        return AccessToken{std::move(token), std::chrono::system_clock::time_point::max(), {}};

    if (const std::string explicit_path = env("GOOGLE_APPLICATION_CREDENTIALS");
        !explicit_path.empty()) {
        // ADC defines this variable as a caller-selected credential file. It is
        // not a path beneath an application-owned root or a privilege boundary.
        // codeql[cpp/path-injection]
        return token_from_adc_file(explicit_path);
    }

    if (const auto path = well_known_adc_path(); path && std::filesystem::exists(*path))
        return token_from_adc_file(*path);

    try {
        return metadata_token();
    } catch (const Error&) {
    }

    throw Error("No usable Google credentials; use ADC, an attached service account, "
                "or Credentials::from(...)");
}

inline std::string compact_error_body(std::string body) {
    constexpr std::size_t max = 8 * 1024;
    if (body.size() > max)
        body.resize(max);
    return trim(std::move(body));
}

// Authenticated GCP REST core --------------------------------------------------

// Core centralises lazy project/zone discovery, bearer headers, a single 401
// refresh, endpoint validation, and HTTP error normalisation. Public Bucket,
// Vm, Operation, and Cloud handles share it through std::shared_ptr.
class Core {
public:
    explicit Core(Config value) : config(std::move(value)) {
        if (config.timeout <= std::chrono::milliseconds::zero())
            throw Error("Config::timeout must be positive");
        if (config.transfer_timeout <= std::chrono::milliseconds::zero())
            throw Error("Config::transfer_timeout must be positive");
        config.storage_endpoint = base_url(std::move(config.storage_endpoint));
        config.compute_endpoint = base_url(std::move(config.compute_endpoint));
        config.batch_endpoint = base_url(std::move(config.batch_endpoint));
        config.logging_endpoint = base_url(std::move(config.logging_endpoint));
        validate_endpoint(config.storage_endpoint, "storage_endpoint");
        validate_endpoint(config.compute_endpoint, "compute_endpoint");
        validate_endpoint(config.batch_endpoint, "batch_endpoint");
        validate_endpoint(config.logging_endpoint, "logging_endpoint");
    }

    [[nodiscard]] std::string project() const {
        std::lock_guard lock(discovery_mutex);
        if (!resolved_project.empty())
            return resolved_project;
        resolved_project = config.project;
        if (resolved_project.empty())
            resolved_project = env("GOOGLE_CLOUD_PROJECT");
        if (resolved_project.empty())
            resolved_project = env("GCLOUD_PROJECT");
        if (resolved_project.empty())
            resolved_project = env("GCP_PROJECT");
        if (resolved_project.empty()) {
            if (const auto value = metadata_get("project/project-id"))
                resolved_project = *value;
        }
        if (resolved_project.empty())
            throw Error("No GCP project configured; set Config::project or GOOGLE_CLOUD_PROJECT");
        return resolved_project;
    }

    [[nodiscard]] std::string zone() const {
        std::lock_guard lock(discovery_mutex);
        if (!resolved_zone.empty())
            return resolved_zone;
        resolved_zone = config.zone;
        if (resolved_zone.empty())
            resolved_zone = env("GOOGLE_CLOUD_ZONE");
        if (resolved_zone.empty())
            resolved_zone = env("GCP_ZONE");
        if (resolved_zone.empty()) {
            if (const auto value = metadata_get("instance/zone"))
                resolved_zone = last_path_segment(*value);
        }
        if (resolved_zone.empty())
            throw Error("No Compute Engine zone configured; set Config::zone or GOOGLE_CLOUD_ZONE");
        return resolved_zone;
    }

    HttpResponse call(HttpRequest request) const {
        if (!request.timeout)
            request.timeout = config.timeout;

        const auto execute = [this](HttpRequest current) {
            const AccessToken token = config.credentials.access_token();
            current.headers.push_back(header("Authorization", "Bearer " + token.value));
            if (current.accept_json)
                current.headers.push_back("Accept: application/json");
            std::string quota = config.quota_project;
            if (quota.empty())
                quota = env("GOOGLE_CLOUD_QUOTA_PROJECT");
            if (quota.empty())
                quota = token.quota_project;
            if (!quota.empty())
                current.headers.push_back(header("X-Goog-User-Project", quota));
            return http(std::move(current));
        };

        HttpResponse response = execute(request);
        if (response.status == 401) {
            config.credentials.invalidate();
            response = execute(std::move(request));
        }
        check(response);
        return response;
    }

    Json json(HttpRequest request) const { return parse_json(call(std::move(request)).body); }

    std::string compute_url(std::string_view zone, std::string path) const {
        return config.compute_endpoint + "/compute/v1/projects/" + encode(project()) + "/zones/" +
               encode(zone) + std::move(path);
    }

    Config config;

private:
    void validate_endpoint(const std::string& endpoint, std::string_view option_name) const {
        if (starts_with(endpoint, "https://"))
            return;
        if (starts_with(endpoint, "http://") && config.allow_insecure_http)
            return;
        throw Error("Config::" + std::string(option_name) +
                    " must use HTTPS (or set allow_insecure_http explicitly)");
    }

    static void check(const HttpResponse& response) {
        if (response.status >= 200 && response.status < 300)
            return;
        const std::string body = compact_error_body(response.body);
        std::string message =
            "Google Cloud request failed with HTTP " + std::to_string(response.status);
        if (!body.empty())
            message += ": " + body;
        throw Error(std::move(message), response.status, body);
    }

    mutable std::mutex discovery_mutex;
    mutable std::string resolved_project;
    mutable std::string resolved_zone;
};

inline Object parse_object(const Json& json) {
    Object result;
    result.name = field(json, "name");
    result.generation = field(json, "generation");
    result.size = unsigned_field(json, "size");
    result.content_type = field(json, "contentType");
    result.updated = field(json, "updated");
    result.etag = field(json, "etag");
    result.crc32c = field(json, "crc32c");
    result.md5_hash = field(json, "md5Hash");
    return result;
}

inline Instance parse_instance(const Json& json) {
    Instance result;
    result.id = field(json, "id");
    result.name = field(json, "name");
    result.zone = last_path_segment(field(json, "zone"));
    result.machine_type = last_path_segment(field(json, "machineType"));
    result.status = field(json, "status");
    result.creation_timestamp = field(json, "creationTimestamp");
    for_each_json(json, "networkInterfaces", [&](const Json& interface) {
        if (result.internal_ip.empty())
            result.internal_ip = field(interface, "networkIP");
        for_each_json(interface, "accessConfigs", [&](const Json& config) {
            if (result.external_ip.empty())
                result.external_ip = field(config, "natIP");
        });
    });
    return result;
}

inline std::string operation_error(const Json& json) {
    const Json* error = json.get("error");
    std::string result;
    if (error)
        for_each_json(*error, "errors", [&](const Json& item) {
            std::string message = field(item, "message");
            if (message.empty())
                message = field(item, "code");
            if (!message.empty()) {
                if (!result.empty())
                    result += "; ";
                result += message;
            }
        });
    if (result.empty())
        result = field(json, "httpErrorMessage");
    const std::string status_code = field(json, "httpErrorStatusCode");
    if (result.empty() && !status_code.empty() && status_code != "0")
        result = "HTTP status " + status_code;
    if (result.empty() && error && error->is_object())
        result = "unknown operation error";
    return result;
}

} // namespace detail

// Low-level GCS and GCE handles ------------------------------------------------

// Bucket uses single-request media uploads. Verified downloads first read
// metadata, pin that exact generation, calculate CRC32C while streaming, and
// preserve the destination unless every step succeeds.
class Bucket {
public:
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    [[nodiscard]] Object stat(std::string_view object) const {
        return detail::parse_object(core_->json(detail::HttpRequest{}.with_url(
            storage("/storage/v1/b/" + detail::encode(name_) + "/o/" + detail::encode(object) +
                    "?fields=name%2Cgeneration%2Csize%2CcontentType%2Cupdated%2Cetag%2Ccrc32c%"
                    "2Cmd5Hash"))));
    }

    [[nodiscard]] ObjectList list(ListOptions options = {}) const {
        ObjectList result;
        result.objects = detail::paginate<Object>(
            options.limit,
            [&](const std::string& page) {
                std::string query = "?maxResults=1000";
                if (!options.prefix.empty())
                    query += "&prefix=" + detail::encode(options.prefix);
                if (!options.delimiter.empty())
                    query += "&delimiter=" + detail::encode(options.delimiter);
                if (options.versions)
                    query += "&versions=true";
                if (!page.empty())
                    query += "&pageToken=" + detail::encode(page);
                return core_->json(detail::HttpRequest{}.with_url(
                    storage("/storage/v1/b/" + detail::encode(name_) + "/o" + query)));
            },
            detail::parse_object,
            [&](const detail::Json& json) {
                detail::for_each_json(json, "prefixes", [&](const detail::Json& prefix) {
                    result.prefixes.push_back(prefix.text());
                });
            });
        return result;
    }

    [[nodiscard]] Object put(std::string_view object, std::string_view bytes,
                             PutOptions options = {}) const {
        const std::string checksum = options.crc32c ? detail::crc32c(bytes) : std::string{};
        Object result =
            detail::parse_object(core_->json(detail::HttpRequest{}
                                                 .with_method("POST")
                                                 .with_url(upload_url(object, options))
                                                 .with_headers(upload_headers(options, checksum))
                                                 .with_body(std::string(bytes))));
        if (!checksum.empty() && checksum != result.crc32c)
            throw Error("Cloud Storage upload checksum mismatch");
        return result;
    }

    [[nodiscard]] Object put_file(std::string_view object, const std::filesystem::path& source,
                                  PutOptions options = {}) const {
        const auto upload = detail::prepare_upload(source, options.crc32c);
        const std::string& checksum = upload->crc32c;
        Object result =
            detail::parse_object(core_->json(detail::HttpRequest{}
                                                 .with_method("POST")
                                                 .with_url(upload_url(object, options))
                                                 .with_headers(upload_headers(options, checksum))
                                                 .with_upload_file(upload)
                                                 .with_timeout(core_->config.transfer_timeout)));
        if (!checksum.empty() && checksum != result.crc32c)
            throw Error("Cloud Storage upload checksum mismatch");
        return result;
    }

    // Verified downloads first pin a generation and require CRC32C metadata, so
    // bytes cannot silently come from a different object version.
    [[nodiscard]] std::string get(std::string_view object, bool verify = true) const {
        return download(object, verify, [](auto&, const auto&) {}).body;
    }

    void get_file(std::string_view object, const std::filesystem::path& destination,
                  bool verify = true) const {
        (void)download(object, verify, [&](auto& request, const Object& metadata) {
            request.download_file = destination;
            request.expected_crc32c =
                verify ? std::optional<std::string>(metadata.crc32c) : std::nullopt;
            request.timeout = core_->config.transfer_timeout;
        });
    }

    void erase(std::string_view object,
               std::optional<std::string> generation = std::nullopt) const {
        std::string query;
        if (generation) {
            query = "?generation=" + detail::encode(*generation) +
                    "&ifGenerationMatch=" + detail::encode(*generation);
        }
        core_->call(detail::HttpRequest{}.with_method("DELETE").with_url(storage(
            "/storage/v1/b/" + detail::encode(name_) + "/o/" + detail::encode(object) + query)));
    }

private:
    friend class Cloud;
    Bucket(std::shared_ptr<detail::Core> core, std::string name)
        : core_(std::move(core)), name_(std::move(name)) {
        if (name_.empty())
            throw Error("Bucket name must not be empty");
    }

    [[nodiscard]] std::string storage(std::string path) const {
        return core_->config.storage_endpoint + std::move(path);
    }

    [[nodiscard]] std::string upload_url(std::string_view object, const PutOptions& options) const {
        std::string url = storage("/upload/storage/v1/b/" + detail::encode(name_) +
                                  "/o?uploadType=media&name=" + detail::encode(object));
        if (options.if_generation_match)
            url += "&ifGenerationMatch=" + detail::encode(*options.if_generation_match);
        return url;
    }

    [[nodiscard]] static std::vector<std::string> upload_headers(const PutOptions& options,
                                                                 const std::string& checksum) {
        std::vector<std::string> headers{detail::header("Content-Type", options.content_type)};
        if (!checksum.empty())
            headers.push_back(detail::header("X-Goog-Hash", "crc32c=" + checksum));
        return headers;
    }

    template <typename Configure>
    [[nodiscard]] detail::HttpResponse download(std::string_view object, bool verify,
                                                Configure configure) const {
        Object metadata;
        if (verify)
            metadata = stat(object);
        require_verification_metadata(metadata, verify);
        std::string query = "?alt=media";
        if (verify)
            query += "&generation=" + detail::encode(metadata.generation);
        detail::HttpRequest request =
            detail::HttpRequest{}
                .with_url(storage("/download/storage/v1/b/" + detail::encode(name_) + "/o/" +
                                  detail::encode(object) + query))
                .with_headers({"Accept-Encoding: gzip"})
                .with_calculate_crc32c(verify)
                .with_accept_json(false);
        configure(request, metadata);
        auto response = core_->call(std::move(request));
        if (verify && response.crc32c != metadata.crc32c)
            throw Error("Cloud Storage download checksum mismatch");
        return response;
    }

    static void require_verification_metadata(const Object& metadata, bool verify) {
        if (!verify)
            return;
        if (metadata.generation.empty() || metadata.crc32c.empty())
            throw Error("Cloud Storage object lacks generation/CRC32C metadata; "
                        "verified download is unavailable");
    }

    std::shared_ptr<detail::Core> core_;
    std::string name_;
};

// A zonal GCE long-running operation. Mutating methods return immediately with
// this handle; wait() performs bounded polling and surfaces operation errors.
class Operation {
public:
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& zone() const noexcept { return zone_; }

    // Waits until Compute Engine says DONE and then verifies operation.error.
    void wait(std::chrono::milliseconds timeout = std::chrono::minutes(10),
              std::chrono::milliseconds poll_interval = std::chrono::seconds(1)) const {
        if (timeout <= std::chrono::milliseconds::zero())
            throw Error("Compute operation wait timeout must be positive");
        if (poll_interval <= std::chrono::milliseconds::zero())
            throw Error("Compute operation poll interval must be positive");
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                throw Error("Timed out waiting for Compute operation '" + name_ + "'");
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const detail::Json json = core_->json(
                detail::HttpRequest{}
                    .with_url(core_->compute_url(zone_, "/operations/" + detail::encode(name_)))
                    .with_timeout(std::max(std::chrono::milliseconds(1),
                                           std::min(core_->config.timeout, remaining))));
            if (detail::field(json, "status") == "DONE") {
                const std::string failure = detail::operation_error(json);
                if (!failure.empty())
                    throw Error("Compute operation failed: " + failure);
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw Error("Timed out waiting for Compute operation '" + name_ + "'");
            std::this_thread::sleep_for(
                std::min(poll_interval, std::chrono::duration_cast<std::chrono::milliseconds>(
                                            deadline - std::chrono::steady_clock::now())));
        }
    }

private:
    friend class Vm;
    friend class Cloud;
    Operation(std::shared_ptr<detail::Core> core, std::string name, std::string zone)
        : core_(std::move(core)), name_(std::move(name)), zone_(std::move(zone)) {
        if (name_.empty())
            throw Error("Malformed Compute operation response: missing name");
    }

    std::shared_ptr<detail::Core> core_;
    std::string name_;
    std::string zone_;
};

// One named VM in Config::zone. start/stop/erase carry idempotency request IDs
// and return an Operation rather than hiding an unbounded wait.
class Vm {
public:
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    [[nodiscard]] Instance get() const {
        return detail::parse_instance(core_->json(detail::HttpRequest{}.with_url(instance_url())));
    }

    [[nodiscard]] std::string status() const { return get().status; }

    [[nodiscard]] Operation start() const { return action("start"); }
    [[nodiscard]] Operation stop(bool discard_local_ssd = false) const {
        return action(std::string("stop?discardLocalSsd=") +
                          (discard_local_ssd ? "true" : "false") +
                          "&requestId=" + detail::random_uuid(),
                      false);
    }
    [[nodiscard]] Operation erase() const { return action("", true); }

private:
    friend class Cloud;
    Vm(std::shared_ptr<detail::Core> core, std::string name)
        : core_(std::move(core)), name_(std::move(name)) {
        if (name_.empty())
            throw Error("VM name must not be empty");
    }

    [[nodiscard]] std::string instance_url() const {
        return core_->compute_url(core_->zone(), "/instances/" + detail::encode(name_));
    }

    [[nodiscard]] Operation action(std::string action, bool deleting = false) const {
        std::string url = instance_url();
        if (deleting) {
            url += "?requestId=" + detail::random_uuid();
        } else if (detail::starts_with(action, "stop?")) {
            url += "/" + action;
        } else {
            url += "/" + action + "?requestId=" + detail::random_uuid();
        }
        const detail::Json json = core_->json(detail::HttpRequest{}
                                                  .with_method(deleting ? "DELETE" : "POST")
                                                  .with_url(std::move(url)));
        return Operation(core_, detail::field(json, "name"), core_->zone());
    }

    std::shared_ptr<detail::Core> core_;
    std::string name_;
};

// Root of the low-level GCP API. Handles copied from Cloud share credentials,
// discovery caches, and endpoint configuration.
class Cloud {
public:
    explicit Cloud(Config config = {}) : core_(std::make_shared<detail::Core>(std::move(config))) {}

    [[nodiscard]] Bucket bucket(std::string name) const { return Bucket(core_, std::move(name)); }

    [[nodiscard]] Vm vm(std::string name) const { return Vm(core_, std::move(name)); }

    [[nodiscard]] Operation create_from_template(std::string name,
                                                 std::string instance_template) const {
        if (name.empty() || instance_template.empty())
            throw Error("VM name and instance template must not be empty");
        if (!detail::starts_with(instance_template, "projects/") &&
            !detail::starts_with(instance_template, "global/"))
            instance_template = "global/instanceTemplates/" + instance_template;
        const std::string request_id = detail::random_uuid();
        const detail::Json json = core_->json(
            detail::HttpRequest{}
                .with_method("POST")
                .with_url(core_->compute_url(core_->zone(), "/instances?sourceInstanceTemplate=" +
                                                                detail::encode(instance_template) +
                                                                "&requestId=" + request_id))
                .with_headers({"Content-Type: application/json"})
                .with_body("{\"name\":" + detail::json_quote(name) + "}"));
        return Operation(core_, detail::field(json, "name"), core_->zone());
    }

    [[nodiscard]] std::vector<Instance> vms(std::size_t limit = 0) const {
        return detail::paginate<Instance>(
            limit,
            [&](const std::string& page) {
                std::string query = "?maxResults=500";
                if (!page.empty())
                    query += "&pageToken=" + detail::encode(page);
                return core_->json(detail::HttpRequest{}.with_url(
                    core_->compute_url(core_->zone(), "/instances" + query)));
            },
            detail::parse_instance, [](const detail::Json&) {});
    }

    [[nodiscard]] std::string project() const { return core_->project(); }
    [[nodiscard]] std::string zone() const { return core_->zone(); }

private:
    friend struct ::cldmux::detail::client_state;
    std::shared_ptr<detail::Core> core_;
};

} // namespace cldmux::gcp
