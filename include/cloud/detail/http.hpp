#pragma once

#include "cloud/detail/json.hpp"

namespace cloud::gcp {
namespace detail {

// libcurl transport ------------------------------------------------------------

struct CurlGlobal {
    CurlGlobal() {
        const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK)
            throw Error("curl_global_init failed");
        const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
        if (!info || !(info->features & CURL_VERSION_THREADSAFE))
            throw Error("cloud.h requires a thread-safe libcurl build");
    }
    // Deliberately process-lifetime: destructor-time cleanup can race clients
    // owned by other static objects.
    ~CurlGlobal() = default;
};

static_assert(LIBCURL_VERSION_NUM >= 0x075400, "cloud.h requires libcurl 7.84.0 or newer");

inline void ensure_curl() {
    static CurlGlobal global;
    (void)global;
}

// One internal HTTP request. Bodies and file streams are mutually exclusive;
// downloads may request streaming CRC32C verification before the destination is
// committed. AwsSignature delegates SigV4 canonicalisation to libcurl.
struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    bool body_present = false;
    std::shared_ptr<UploadSource> upload_file;
    std::optional<std::filesystem::path> download_file;
    std::optional<std::string> expected_crc32c;
    bool calculate_crc32c = false;
    bool accept_json = true;
    bool no_proxy = false;
    std::optional<std::chrono::milliseconds> timeout;
    std::chrono::milliseconds connect_timeout{10'000};
    // In-memory responses are deliberately bounded. Successful file downloads
    // stream without this ceiling; their error bodies remain bounded below.
    std::size_t max_response_bytes = 16 * 1024 * 1024;
    struct AwsSignature {
        std::string access_key_id;
        std::string secret_access_key;
        std::string session_token;
        std::string region;
        std::string service;
    };
    std::optional<AwsSignature> aws_signature;

    // These rvalue-qualified setters keep request construction readable without
    // requiring C++20 designated initialisers. HttpRequest is an implementation
    // detail, so the public aggregate-based API remains unchanged.
    HttpRequest&& with_method(std::string value) && {
        method = std::move(value);
        return std::move(*this);
    }
    HttpRequest&& with_url(std::string value) && {
        url = std::move(value);
        return std::move(*this);
    }
    HttpRequest&& with_headers(std::vector<std::string> value) && {
        headers = std::move(value);
        return std::move(*this);
    }
    HttpRequest&& with_body(std::string value) && {
        body = std::move(value);
        body_present = true;
        return std::move(*this);
    }
    HttpRequest&& with_upload_file(std::shared_ptr<UploadSource> value) && {
        upload_file = std::move(value);
        return std::move(*this);
    }
    HttpRequest&& with_calculate_crc32c(bool value) && {
        calculate_crc32c = value;
        return std::move(*this);
    }
    HttpRequest&& with_accept_json(bool value) && {
        accept_json = value;
        return std::move(*this);
    }
    HttpRequest&& with_no_proxy(bool value) && {
        no_proxy = value;
        return std::move(*this);
    }
    HttpRequest&& with_timeout(std::chrono::milliseconds value) && {
        timeout = value;
        return std::move(*this);
    }
    HttpRequest&& with_connect_timeout(std::chrono::milliseconds value) && {
        connect_timeout = value;
        return std::move(*this);
    }
    HttpRequest&& with_max_response_bytes(std::size_t value) && {
        max_response_bytes = value;
        return std::move(*this);
    }
};

// Response headers use lowercase names. crc32c is populated only when the
// request asked the write callback to calculate it.
struct HttpResponse {
    long status = 0;
    std::string body;
    std::string crc32c;
    std::map<std::string, std::string, std::less<>> headers;
};

// libcurl callbacks are C boundaries and must not throw. They record exceptions
// and I/O failures here so http() can rethrow after curl_easy_perform returns.
struct WriteSink {
    std::string* text = nullptr;
    std::ofstream* file = nullptr;
    long* status = nullptr;
    std::size_t max_response_bytes = 0;
    std::size_t received = 0;
    bool checksum = false;
    std::uint32_t crc = 0;
    bool io_error = false;
    bool limit_exceeded = false;
    std::exception_ptr exception;
};

struct HeaderSink {
    std::map<std::string, std::string, std::less<>>* headers = nullptr;
    long* status = nullptr;
    std::size_t received = 0;
    bool limit_exceeded = false;
    std::exception_ptr exception;
};

inline constexpr std::size_t max_error_response_bytes = 64 * 1024;
inline constexpr std::size_t max_response_header_bytes = 256 * 1024;

inline std::size_t header_callback(char* data, std::size_t size, std::size_t count,
                                   void* userdata) noexcept {
    auto& sink = *static_cast<HeaderSink*>(userdata);
    try {
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
            return 0;
        const std::size_t bytes = size * count;
        if (bytes > max_response_header_bytes -
                        std::min(sink.received, max_response_header_bytes)) {
            sink.limit_exceeded = true;
            return 0;
        }
        sink.received += bytes;
        std::string_view line(data, bytes);
        if (starts_with(line, "HTTP/")) {
            const auto space = line.find(' ');
            if (space != std::string_view::npos && space + 4 <= line.size() &&
                is_ascii_digit(line[space + 1]) && is_ascii_digit(line[space + 2]) &&
                is_ascii_digit(line[space + 3])) {
                *sink.status = static_cast<long>((line[space + 1] - '0') * 100 +
                                                 (line[space + 2] - '0') * 10 +
                                                 (line[space + 3] - '0'));
            }
        }
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string name(line.substr(0, colon));
            std::transform(name.begin(), name.end(), name.begin(), ascii_lower);
            std::string value(line.substr(colon + 1));
            value = trim(std::move(value));
            (*sink.headers)[std::move(name)] = std::move(value);
        }
        return bytes;
    } catch (...) {
        sink.exception = std::current_exception();
        return 0;
    }
}

inline std::size_t write_callback(char* data, std::size_t size, std::size_t count,
                                  void* userdata) noexcept {
    auto& sink = *static_cast<WriteSink*>(userdata);
    try {
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
            sink.io_error = true;
            return 0;
        }
        const std::size_t bytes = size * count;
        const bool successful = sink.status && *sink.status >= 200 && *sink.status < 300;
        const bool unlimited_file = sink.file && successful;
        const std::size_t limit = successful
                                      ? sink.max_response_bytes
                                      : std::min(sink.max_response_bytes, max_error_response_bytes);
        if (!unlimited_file &&
            bytes > limit - std::min(sink.received, limit)) {
            sink.limit_exceeded = true;
            return 0;
        }
        if (!unlimited_file)
            sink.received += bytes;
        if (sink.file) {
            if (bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
                sink.io_error = true;
                return 0;
            }
            sink.file->write(data, static_cast<std::streamsize>(bytes));
            if (!*sink.file) {
                sink.io_error = true;
                return 0;
            }
        } else {
            sink.text->append(data, bytes);
        }
        if (sink.checksum)
            sink.crc = crc32c_update(sink.crc, reinterpret_cast<const unsigned char*>(data), bytes);
        return bytes;
    } catch (...) {
        sink.exception = std::current_exception();
        return 0;
    }
}

struct ReadSource {
    std::ifstream* stream = nullptr;
    curl_off_t remaining = 0;
    bool io_error = false;
    std::exception_ptr exception;
};

inline std::size_t read_callback(char* data, std::size_t size, std::size_t count,
                                 void* userdata) noexcept {
    auto& source = *static_cast<ReadSource*>(userdata);
    try {
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
            source.io_error = true;
            return CURL_READFUNC_ABORT;
        }
        std::size_t capacity = size * count;
        if (source.remaining <= 0)
            return 0;
        if (static_cast<std::uintmax_t>(source.remaining) < capacity)
            capacity = static_cast<std::size_t>(source.remaining);
        if (capacity > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            source.io_error = true;
            return CURL_READFUNC_ABORT;
        }
        source.stream->read(data, static_cast<std::streamsize>(capacity));
        if (source.stream->bad()) {
            source.io_error = true;
            return CURL_READFUNC_ABORT;
        }
        const auto received = source.stream->gcount();
        if (received <= 0) {
            source.io_error = true;
            return CURL_READFUNC_ABORT;
        }
        source.remaining -= static_cast<curl_off_t>(received);
        return static_cast<std::size_t>(received);
    } catch (...) {
        source.exception = std::current_exception();
        return CURL_READFUNC_ABORT;
    }
}

inline void curl_check(CURLcode code, std::string_view action) {
    if (code != CURLE_OK)
        throw Error("libcurl " + std::string(action) + " failed: " + curl_easy_strerror(code));
}

template <typename Value> inline void curl_set(CURL* curl, CURLoption option, Value value) {
    curl_check(curl_easy_setopt(curl, option, value), "configuration");
}

inline long curl_milliseconds(std::chrono::milliseconds value, std::string_view name) {
    if (value <= std::chrono::milliseconds::zero() ||
        value.count() > std::numeric_limits<long>::max())
        throw Error(std::string(name) + " must be a positive libcurl duration");
    return static_cast<long>(value.count());
}

inline curl_off_t curl_size(std::size_t value) {
    if (value > static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max()))
        throw Error("HTTP request body is too large for libcurl");
    return static_cast<curl_off_t>(value);
}

inline std::string read_small_file(const std::filesystem::path& path,
                                   std::size_t max_bytes = 64 * 1024) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::string result(max_bytes, '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<std::size_t>(input.gcount()));
    return result;
}

inline void remove_noexcept(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    (void)std::filesystem::remove(path, ignored);
}

// Removes an uncommitted download path during stack unwinding. dismiss() is
// called only after verification and destination replacement succeed.
class TemporaryPathGuard {
public:
    TemporaryPathGuard() = default;
    explicit TemporaryPathGuard(std::filesystem::path path)
        : path_(std::move(path)), active_(true) {}
    TemporaryPathGuard(const TemporaryPathGuard&) = delete;
    TemporaryPathGuard& operator=(const TemporaryPathGuard&) = delete;
    ~TemporaryPathGuard() {
        if (active_)
            remove_noexcept(path_);
    }
    void dismiss() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_ = false;
};

// Execute exactly one HTTP transaction; provider-level helpers own retries.
// File downloads stream into a sibling temporary file, verify any expected
// checksum, and only then replace the destination. The compatibility fallback
// restores an existing destination if a platform cannot replace it atomically.
inline HttpResponse http(HttpRequest request) {
    ensure_curl();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl)
        throw Error("curl_easy_init failed");

    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_set(curl.get(), CURLOPT_ERRORBUFFER, error_buffer);
    curl_set(curl.get(), CURLOPT_URL, request.url.c_str());
    curl_set(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_set(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_set(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_set(curl.get(), CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_set(curl.get(), CURLOPT_TIMEOUT_MS,
             curl_milliseconds(request.timeout.value_or(std::chrono::milliseconds(60'000)),
                               "HTTP timeout"));
    curl_set(curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
             curl_milliseconds(request.connect_timeout, "HTTP connect timeout"));
    curl_set(curl.get(), CURLOPT_USERAGENT, "cloud.h/" CLOUD_H_VERSION);
    if (request.no_proxy)
        curl_set(curl.get(), CURLOPT_NOPROXY, "*");

    std::string aws_scope;
    std::string aws_user;
    if (request.aws_signature) {
        const auto& signature = *request.aws_signature;
        if (signature.access_key_id.empty() || signature.secret_access_key.empty() ||
            signature.region.empty() || signature.service.empty())
            throw Error("Incomplete AWS request-signing configuration");
        aws_scope = "aws:amz:" + signature.region + ':' + signature.service;
        aws_user = signature.access_key_id + ':' + signature.secret_access_key;
        curl_set(curl.get(), CURLOPT_AWS_SIGV4, aws_scope.c_str());
        curl_set(curl.get(), CURLOPT_USERPWD, aws_user.c_str());
        if (!signature.session_token.empty())
            request.headers.push_back(header("X-Amz-Security-Token", signature.session_token));
    }

    curl_slist* raw_headers = nullptr;
    for (const auto& header : request.headers) {
        curl_slist* appended = curl_slist_append(raw_headers, header.c_str());
        if (!appended) {
            curl_slist_free_all(raw_headers);
            throw Error("libcurl could not allocate request headers");
        }
        raw_headers = appended;
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers,
                                                                        &curl_slist_free_all);
    if (headers)
        curl_set(curl.get(), CURLOPT_HTTPHEADER, headers.get());

    ReadSource upload_source;
    if (request.upload_file) {
        auto& upload = *request.upload_file->stream;
        upload.clear();
        upload.seekg(0, std::ios::beg);
        if (!upload)
            throw Error("Cannot rewind upload file: " + request.upload_file->path.string());
        upload_source.stream = &upload;
        upload_source.remaining = request.upload_file->size;
        curl_set(curl.get(), CURLOPT_READFUNCTION, &read_callback);
        curl_set(curl.get(), CURLOPT_READDATA, &upload_source);
        if (request.method == "POST") {
            curl_set(curl.get(), CURLOPT_POST, 1L);
            curl_set(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, request.upload_file->size);
        } else {
            // CURLOPT_UPLOAD selects HTTP PUT by default. Keeping the method
            // explicit for any future streamed verb avoids silently turning a
            // provider upload into the POST historically used by GCS.
            curl_set(curl.get(), CURLOPT_UPLOAD, 1L);
            curl_set(curl.get(), CURLOPT_INFILESIZE_LARGE, request.upload_file->size);
            if (request.method != "PUT")
                curl_set(curl.get(), CURLOPT_CUSTOMREQUEST, request.method.c_str());
        }
    } else if (request.method == "POST") {
        curl_set(curl.get(), CURLOPT_POST, 1L);
        curl_set(curl.get(), CURLOPT_POSTFIELDS, request.body.data());
        curl_set(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, curl_size(request.body.size()));
    } else if (request.method == "HEAD") {
        curl_set(curl.get(), CURLOPT_NOBODY, 1L);
    } else if (request.method != "GET") {
        curl_set(curl.get(), CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (request.body_present) {
            curl_set(curl.get(), CURLOPT_POSTFIELDS, request.body.data());
            curl_set(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, curl_size(request.body.size()));
        }
    }

    HttpResponse response;
    std::filesystem::path temporary;
    std::optional<TemporaryPathGuard> temporary_guard;
    std::ofstream output;
    if (request.download_file) {
        const auto parent = request.download_file->parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent);
        temporary = *request.download_file;
        temporary += ".cloud-part-" + random_uuid();
        temporary_guard.emplace(temporary);
        output.open(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw Error("Cannot open download destination: " + temporary.string());
    }
    WriteSink sink;
    sink.text = request.download_file ? nullptr : &response.body;
    sink.file = request.download_file ? &output : nullptr;
    sink.status = &response.status;
    sink.max_response_bytes = request.max_response_bytes;
    sink.checksum = request.calculate_crc32c || request.expected_crc32c.has_value();
    curl_set(curl.get(), CURLOPT_WRITEFUNCTION, &write_callback);
    curl_set(curl.get(), CURLOPT_WRITEDATA, &sink);
    HeaderSink header_sink;
    header_sink.headers = &response.headers;
    header_sink.status = &response.status;
    curl_set(curl.get(), CURLOPT_HEADERFUNCTION, &header_callback);
    curl_set(curl.get(), CURLOPT_HEADERDATA, &header_sink);

    const CURLcode code = curl_easy_perform(curl.get());
    const CURLcode info_code =
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
    bool output_ok = true;
    if (output.is_open()) {
        output.flush();
        output_ok = output.good();
        output.close();
        output_ok = output_ok && !output.fail();
    }
    if (sink.checksum)
        response.crc32c = crc32c_base64(sink.crc);

    if (header_sink.limit_exceeded)
        throw Error("HTTP response headers exceeded the private byte limit", response.status);
    if (sink.limit_exceeded) {
        const std::string retained =
            sink.text ? sink.text->substr(0, 8 * 1024) : read_small_file(temporary, 8 * 1024);
        throw Error("HTTP response body exceeded the private byte limit", response.status,
                    retained);
    }

    const auto callback_error = [&]() -> std::optional<std::string> {
        if (sink.exception)
            return "HTTP response callback threw an exception";
        if (header_sink.exception)
            return "HTTP header callback threw an exception";
        if (sink.io_error)
            return "HTTP response could not be written";
        if (upload_source.exception)
            return "Upload callback threw an exception";
        if (upload_source.io_error)
            return "Upload file could not be read";
        return std::nullopt;
    }();
    if (callback_error) {
        throw Error(*callback_error);
    }

    if (code != CURLE_OK) {
        const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(code);
        throw Error("HTTP transport failed: " + detail);
    }
    if (info_code != CURLE_OK) {
        curl_check(info_code, "response status lookup");
    }
    if (!output_ok) {
        throw Error("Failed to flush completed download to disk");
    }

    if (request.download_file) {
        if (response.status < 200 || response.status >= 300) {
            response.body = read_small_file(temporary);
        } else if (request.expected_crc32c && response.crc32c != *request.expected_crc32c) {
            throw Error("Cloud Storage download checksum mismatch");
        } else {
            std::error_code ec;
            std::filesystem::rename(temporary, *request.download_file, ec);
            if (ec && std::filesystem::exists(*request.download_file)) {
                // Windows does not replace an existing destination with rename(). Keep
                // the old file recoverable until the new one is safely in place.
                std::filesystem::path backup = *request.download_file;
                backup += ".cloud-backup-" + random_uuid();
                std::error_code backup_error;
                std::filesystem::rename(*request.download_file, backup, backup_error);
                if (!backup_error) {
                    ec.clear();
                    std::filesystem::rename(temporary, *request.download_file, ec);
                    if (!ec) {
                        std::filesystem::remove(backup, backup_error);
                    } else {
                        std::error_code restore_error;
                        std::filesystem::rename(backup, *request.download_file, restore_error);
                    }
                }
            }
            if (ec) {
                throw Error("Cannot move completed download into place: " + ec.message());
            }
            temporary_guard->dismiss();
        }
    }
    return response;
}

} // namespace detail
} // namespace cloud::gcp
