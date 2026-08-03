#pragma once

#include "cloud/plan.hpp"

namespace cloud {
namespace detail {

// Shared provider state, transports, and wire helpers -------------------------

// Shared immutable configuration plus the few synchronised caches. Handles keep
// this state alive, so callbacks and credential caches outlive the client object
// from which a job/storage/compute handle was copied.
struct client_state {
    cloud::config config;
    gcp::Cloud raw;
    gcp::Credentials azure_batch_auth;
    gcp::Credentials azure_storage_auth;
    gcp::Credentials azure_management_auth;
    mutable std::mutex price_mutex;
    mutable std::map<std::string, std::pair<std::chrono::steady_clock::time_point, double>,
                     std::less<>>
        price_cache;

    static gcp::Config low_level(const cloud::config& value) {
        gcp::Config out;
        out.project = value.project;
        out.zone = configured_zone(value, "gcp");
        out.credentials = value.auth.for_scope("https://www.googleapis.com/auth/cloud-platform");
        out.timeout = value.request_timeout;
        out.transfer_timeout = value.transfer_timeout;
        out.allow_insecure_http = value.allow_insecure_http;
        out.storage_endpoint = value.storage_endpoint;
        out.compute_endpoint = value.compute_endpoint;
        out.batch_endpoint = value.batch_endpoint;
        out.logging_endpoint = value.logging_endpoint;
        return out;
    }

    explicit client_state(cloud::config value)
        : config(std::move(value)), raw(low_level(config)),
          azure_batch_auth((config.azure.auth ? *config.azure.auth : config.auth)
                               .for_scope("https://batch.core.windows.net/.default")),
          azure_storage_auth(
              (config.azure.storage_auth
                   ? *config.azure.storage_auth
                   : config.azure.auth ? *config.azure.auth : config.auth)
                  .for_scope("https://storage.azure.com/.default")),
          azure_management_auth(
              (config.azure.management_auth
                   ? *config.azure.management_auth
                   : config.azure.auth ? *config.azure.auth : config.auth)
                  .for_scope("https://management.azure.com/.default")) {
        if (config.poll_interval <= std::chrono::milliseconds::zero() ||
            config.final_log_delay < std::chrono::milliseconds::zero() ||
            config.final_log_timeout < config.final_log_delay ||
            config.cleanup_timeout <= std::chrono::milliseconds::zero() ||
            config.price_cache_ttl <= std::chrono::milliseconds::zero() ||
            config.spot_price_cache_ttl <= std::chrono::milliseconds::zero())
            throw error("Polling and cleanup durations must be valid");
    }

    [[nodiscard]] std::shared_ptr<gcp::detail::Core> core() const { return raw.core_; }

    [[nodiscard]] const gcp::Credentials& azure_credentials() const { return azure_batch_auth; }
    [[nodiscard]] const gcp::Credentials& azure_storage_credentials() const {
        return azure_storage_auth;
    }
    [[nodiscard]] const gcp::Credentials& azure_management_credentials() const {
        return azure_management_auth;
    }
};

inline bool retryable(long status) {
    return status == 0 || status == 408 || status == 429 || (status >= 500 && status < 600);
}

inline std::chrono::milliseconds bounded_retry_delay(
    int attempt, std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point now) {
    auto delay = std::chrono::milliseconds(100 * (1 << attempt));
    if (deadline != std::chrono::steady_clock::time_point::max())
        delay = std::min(
            delay, std::max(std::chrono::milliseconds::zero(),
                            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    return delay;
}

inline void validate_endpoint(const client_state& client, std::string_view value,
                              std::string_view name) {
    if (gcp::detail::starts_with(value, "https://"))
        return;
    if (gcp::detail::starts_with(value, "http://") && client.config.allow_insecure_http)
        return;
    throw error(std::string(name) + " must use HTTPS (or set allow_insecure_http explicitly)");
}

inline std::string endpoint_origin(std::string_view value) {
    const auto scheme = value.find("://");
    if (scheme == std::string_view::npos)
        throw error("Malformed HTTP endpoint");
    const auto end = value.find_first_of("/?#", scheme + 3);
    std::string result(value.substr(0, end));
    std::transform(result.begin(), result.end(), result.begin(), gcp::detail::ascii_lower);
    return result;
}

inline std::string aws_endpoint(const client_state& client, std::string_view configured,
                                std::string_view service, std::string_view region) {
    std::string value =
        configured.empty()
            ? "https://" + std::string(service) + '.' + std::string(region) +
                  (gcp::detail::starts_with(region, "cn-") ? ".amazonaws.com.cn" : ".amazonaws.com")
            : gcp::detail::base_url(std::string(configured));
    validate_endpoint(client, value, "AWS endpoint");
    return value;
}

inline gcp::detail::HttpRequest::AwsSignature
aws_signature(const client_state& client, std::string region, std::string service) {
    // A refresh callback wins over explicit fields, which win over the standard
    // environment variables. Profiles, SSO, ECS, and EC2 metadata are outside
    // this small header and can be adapted through the callback.
    cloud::aws_credentials supplied;
    if (client.config.aws.credentials) {
        supplied = client.config.aws.credentials();
    } else if (!client.config.aws.access_key_id.empty() ||
               !client.config.aws.secret_access_key.empty() ||
               !client.config.aws.session_token.empty()) {
        supplied = {client.config.aws.access_key_id, client.config.aws.secret_access_key,
                    client.config.aws.session_token};
    } else {
        supplied = {gcp::detail::env("AWS_ACCESS_KEY_ID"),
                    gcp::detail::env("AWS_SECRET_ACCESS_KEY"),
                    gcp::detail::env("AWS_SESSION_TOKEN")};
    }
    auto access = std::move(supplied.access_key_id);
    auto secret = std::move(supplied.secret_access_key);
    auto token = std::move(supplied.session_token);
    if (access.empty() || secret.empty())
        throw error("AWS credentials require an access key ID and secret access key");
    gcp::detail::HttpRequest::AwsSignature signature;
    signature.access_key_id = std::move(access);
    signature.secret_access_key = std::move(secret);
    signature.session_token = std::move(token);
    signature.region = std::move(region);
    signature.service = std::move(service);
    return signature;
}

inline void check_response(std::string_view provider, const gcp::detail::HttpResponse& response) {
    if (response.status >= 200 && response.status < 300)
        return;
    const std::string body = gcp::detail::compact_error_body(response.body);
    std::string message =
        std::string(provider) + " request failed with HTTP " + std::to_string(response.status);
    if (!body.empty())
        message += ": " + body;
    throw error(std::move(message), response.status, body);
}

inline gcp::detail::HttpResponse aws_call(
    const client_state& client, gcp::detail::HttpRequest request, std::string region,
    std::string service, bool retry = true,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    // Each attempt is freshly SigV4-signed because credentials may be temporary.
    // Callers disable automatic replay for mutations whose acceptance is ambiguous.
    request.aws_signature = aws_signature(client, std::move(region), std::move(service));
    const auto base_timeout = request.timeout.value_or(client.config.request_timeout);
    for (int attempt = 0;; ++attempt) {
        auto current = request;
        current.timeout = base_timeout;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                throw error("Cloud operation deadline exceeded");
            current.timeout = std::max(
                std::chrono::milliseconds(1),
                std::min(base_timeout,
                         std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        }
        try {
            auto response = gcp::detail::http(std::move(current));
            check_response("AWS", response);
            return response;
        } catch (const error& failure) {
            if (!retry || attempt == 3 || !retryable(failure.http_status()))
                throw;
        }
        const auto backoff =
            bounded_retry_delay(attempt, deadline, std::chrono::steady_clock::now());
        if (backoff <= std::chrono::milliseconds::zero())
            throw error("Cloud operation deadline exceeded");
        std::this_thread::sleep_for(backoff);
    }
}

inline std::string http_date() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &now) != 0)
        throw error("Cannot create Azure request date");
#else
    if (!gmtime_r(&now, &utc))
        throw error("Cannot create Azure request date");
#endif
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::put_time(&utc, "%a, %d %b %Y %H:%M:%S GMT");
    return out.str();
}

inline std::string iso_time() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &now) != 0)
        throw error("Cannot create UTC request time");
#else
    if (!gmtime_r(&now, &utc))
        throw error("Cannot create UTC request time");
#endif
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

enum class azure_service { batch, storage, management };

inline const gcp::Credentials& azure_credentials(const client_state& client,
                                                 azure_service service) {
    if (service == azure_service::storage)
        return client.azure_storage_credentials();
    if (service == azure_service::management)
        return client.azure_management_credentials();
    return client.azure_credentials();
}

inline gcp::detail::HttpResponse azure_service_call(
    const client_state& client, gcp::detail::HttpRequest request, azure_service service,
    bool retry = true,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    // Batch, Blob Storage, and Resource Manager use distinct token audiences
    // and request headers. One 401 invalidates only the affected token cache.
    if (request.url.empty())
        throw error("Azure request URL must not be empty");
    validate_endpoint(client, request.url, "Azure endpoint");
    if (service == azure_service::management) {
        const std::string configured =
            gcp::detail::base_url(client.config.azure.management_endpoint);
        if (configured.empty() || endpoint_origin(request.url) != endpoint_origin(configured))
            throw error("Azure Resource Manager response URL changed origin");
    }
    const auto& credentials = azure_credentials(client, service);
    const auto base_timeout = request.timeout.value_or(client.config.request_timeout);
    for (int attempt = 0;; ++attempt) {
        auto current = request;
        current.timeout = base_timeout;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                throw error("Cloud operation deadline exceeded");
            current.timeout = std::max(
                std::chrono::milliseconds(1),
                std::min(base_timeout,
                         std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        }
        const auto token = credentials.access_token();
        current.headers.push_back(gcp::detail::header("Authorization", "Bearer " + token.value));
        if (service == azure_service::batch) {
            current.headers.push_back(gcp::detail::header("ocp-date", http_date()));
            current.headers.push_back(
                gcp::detail::header("client-request-id", gcp::detail::random_uuid()));
            current.headers.push_back("return-client-request-id: true");
        } else if (service == azure_service::storage) {
            current.headers.push_back(gcp::detail::header("x-ms-date", http_date()));
            current.headers.push_back("x-ms-version: 2023-11-03");
            current.headers.push_back(
                gcp::detail::header("x-ms-client-request-id", gcp::detail::random_uuid()));
        } else {
            current.headers.push_back(
                gcp::detail::header("x-ms-client-request-id", gcp::detail::random_uuid()));
        }
        if (current.accept_json)
            current.headers.push_back("Accept: application/json");
        try {
            auto response = gcp::detail::http(std::move(current));
            if (response.status == 401 && attempt == 0) {
                credentials.invalidate();
                continue;
            }
            check_response("Azure", response);
            return response;
        } catch (const error& failure) {
            if (!retry || attempt == 3 || !retryable(failure.http_status()))
                throw;
        }
        const auto backoff =
            bounded_retry_delay(attempt, deadline, std::chrono::steady_clock::now());
        if (backoff <= std::chrono::milliseconds::zero())
            throw error("Cloud operation deadline exceeded");
        std::this_thread::sleep_for(backoff);
    }
}

inline gcp::detail::HttpResponse azure_call(
    const client_state& client, gcp::detail::HttpRequest request, bool retry = true,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    return azure_service_call(client, std::move(request), azure_service::batch, retry, deadline);
}

inline gcp::detail::HttpResponse azure_storage_call(
    const client_state& client, gcp::detail::HttpRequest request, bool retry = true,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    return azure_service_call(client, std::move(request), azure_service::storage, retry, deadline);
}

inline gcp::detail::HttpResponse azure_management_call(
    const client_state& client, gcp::detail::HttpRequest request, bool retry = true,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    return azure_service_call(client, std::move(request), azure_service::management, retry,
                              deadline);
}

// Provider object APIs use XML and path-oriented object names. These small
// helpers implement only the fixed schemas consumed below; users never author
// JSON or XML configuration.
inline std::string encode_path(std::string_view value) {
    std::string out;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto slash = value.find('/', begin);
        out += gcp::detail::encode(value.substr(begin, slash - begin));
        if (slash == std::string_view::npos)
            break;
        out.push_back('/');
        begin = slash + 1;
    }
    return out;
}

inline int hex_digit(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

inline std::string percent_decode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            out.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size())
            throw error("Malformed percent-encoded provider response");
        const int high = hex_digit(value[i + 1]);
        const int low = hex_digit(value[i + 2]);
        if (high < 0 || low < 0)
            throw error("Malformed percent-encoded provider response");
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

inline std::string xml_unescape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] != '&') {
            out.push_back(value[i++]);
            continue;
        }
        const auto semicolon = value.find(';', i + 1);
        if (semicolon == std::string_view::npos)
            throw error("Malformed XML entity in provider response");
        const auto entity = value.substr(i + 1, semicolon - i - 1);
        if (entity == "amp")
            out.push_back('&');
        else if (entity == "lt")
            out.push_back('<');
        else if (entity == "gt")
            out.push_back('>');
        else if (entity == "quot")
            out.push_back('"');
        else if (entity == "apos")
            out.push_back('\'');
        else
            throw error("Unsupported XML entity in provider response");
        i = semicolon + 1;
    }
    return out;
}

inline std::string xml_raw_field(std::string_view xml, std::string_view tag) {
    const std::string open = '<' + std::string(tag) + '>';
    const std::string close = "</" + std::string(tag) + '>';
    const auto begin = xml.find(open);
    if (begin == std::string_view::npos)
        return {};
    const auto content = begin + open.size();
    const auto end = xml.find(close, content);
    if (end == std::string_view::npos)
        throw error("Malformed XML provider response: missing </" + std::string(tag) + '>');
    return std::string(xml.substr(content, end - content));
}

inline std::string xml_field(std::string_view xml, std::string_view tag) {
    return xml_unescape(xml_raw_field(xml, tag));
}

struct xml_element_value {
    std::string attributes;
    std::string text;
};

// Some Azure list responses annotate an element as percent-encoded, for
// example <Name Encoded="true">. The simpler xml_field() intentionally handles
// only exact, attribute-free elements; this helper keeps the exceptional schema
// explicit rather than turning the compact XML parser into a general parser.
inline std::optional<xml_element_value> xml_element(std::string_view xml,
                                                     std::string_view tag) {
    const std::string prefix = '<' + std::string(tag);
    const std::string close = "</" + std::string(tag) + '>';
    std::size_t cursor = 0;
    while (true) {
        const auto begin = xml.find(prefix, cursor);
        if (begin == std::string_view::npos)
            return std::nullopt;
        const auto boundary = begin + prefix.size();
        if (boundary < xml.size() &&
            (xml[boundary] == '>' || gcp::detail::is_ascii_space(xml[boundary]))) {
            const auto open_end = xml.find('>', boundary);
            if (open_end == std::string_view::npos)
                throw error("Malformed XML provider response: unterminated <" +
                            std::string(tag) + '>');
            const auto end = xml.find(close, open_end + 1);
            if (end == std::string_view::npos)
                throw error("Malformed XML provider response: missing </" + std::string(tag) +
                            '>');
            return xml_element_value{
                std::string(xml.substr(boundary, open_end - boundary)),
                xml_unescape(xml.substr(open_end + 1, end - open_end - 1))};
        }
        cursor = boundary;
    }
}

inline std::string azure_xml_name(std::string_view xml) {
    const auto value = xml_element(xml, "Name");
    if (!value)
        throw error("Malformed Azure Blob list response: missing Name");
    constexpr std::string_view attribute = "Encoded=\"true\"";
    const auto position = value->attributes.find(attribute);
    const bool encoded =
        position != std::string::npos &&
        (position == 0 || gcp::detail::is_ascii_space(value->attributes[position - 1])) &&
        (position + attribute.size() == value->attributes.size() ||
         gcp::detail::is_ascii_space(value->attributes[position + attribute.size()]));
    return encoded ? percent_decode(value->text) : value->text;
}

inline std::vector<std::string_view> xml_blocks(std::string_view xml, std::string_view tag) {
    const std::string open = '<' + std::string(tag) + '>';
    const std::string close = "</" + std::string(tag) + '>';
    std::vector<std::string_view> result;
    std::size_t cursor = 0;
    while (true) {
        const auto begin = xml.find(open, cursor);
        if (begin == std::string_view::npos)
            return result;
        const auto content = begin + open.size();
        std::size_t scan = content;
        std::size_t end = std::string_view::npos;
        unsigned depth = 1;
        while (depth != 0) {
            const auto nested = xml.find(open, scan);
            const auto closing = xml.find(close, scan);
            if (closing == std::string_view::npos)
                throw error("Malformed XML provider response: missing </" + std::string(tag) +
                            '>');
            if (nested != std::string_view::npos && nested < closing) {
                ++depth;
                scan = nested + open.size();
            } else {
                --depth;
                end = closing;
                scan = closing + close.size();
            }
        }
        result.push_back(xml.substr(content, end - content));
        cursor = scan;
    }
}

inline std::uint64_t unsigned_text(std::string_view value, std::string_view field) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw error("Malformed provider " + std::string(field));
    return result;
}

inline std::string response_header(const gcp::detail::HttpResponse& response,
                                   std::string_view name) {
    const auto found = response.headers.find(name);
    return found == response.headers.end() ? std::string{} : found->second;
}

inline std::vector<std::string> conditional_headers(const put_options& options) {
    if (!options.if_generation_match)
        return {};
    if (*options.if_generation_match == "0")
        return {"If-None-Match: *"};
    return {gcp::detail::header("If-Match", *options.if_generation_match)};
}


} // namespace detail
} // namespace cloud
