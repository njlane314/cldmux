// cloud.hpp - small, direct cloud jobs for C++20.
// C++20, libcurl >= 7.84, and nothing else. Link with: -lcurl -pthread
//
// GCP Batch is the first backend. It runs a container on temporary compute,
// polls Cloud Logging, and cleans up the job. GCS storage and raw GCE controls
// are included. AWS/Azure requests fail explicitly until those backends exist.

#ifndef CLOUD_HPP_INCLUDED
#define CLOUD_HPP_INCLUDED

#define CLOUD_HPP_VERSION "0.1.0"
#define CLOUD_HPP_VERSION_NUM 0x000100

/*
 * Headers
 */

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace cloud { namespace detail { struct client_state; } }
namespace cloud::gcp {

inline constexpr std::string_view version = CLOUD_HPP_VERSION;

/*
 * GCP configuration and public types
 */

class Error : public std::runtime_error {
 public:
  explicit Error(std::string message, long http_status = 0, std::string response = {})
      : std::runtime_error(std::move(message)),
        http_status_(http_status),
        response_(std::move(response)) {}

  [[nodiscard]] long http_status() const noexcept { return http_status_; }
  [[nodiscard]] const std::string& response() const noexcept { return response_; }

 private:
  long http_status_ = 0;
  std::string response_;
};

struct AccessToken {
  std::string value;
  std::chrono::system_clock::time_point expires_at = std::chrono::system_clock::time_point::max();
  std::string quota_project;
};

namespace detail {
AccessToken automatic_token();
AccessToken metadata_token();
}  // namespace detail

class Credentials {
 public:
  using Provider = std::function<AccessToken()>;

  Credentials() : Credentials(automatic()) {}

  static Credentials automatic() { return from([] { return detail::automatic_token(); }); }
  static Credentials metadata() { return from([] { return detail::metadata_token(); }); }

  static Credentials bearer(std::string token) {
    if (token.empty()) throw Error("Bearer token must not be empty");
    return from([token = std::move(token)] {
      return AccessToken{token, std::chrono::system_clock::time_point::max(), {}};
    });
  }

  static Credentials from(Provider provider) {
    if (!provider) throw Error("Credential provider must not be empty");
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
      if (fresh.value.empty()) throw Error("Credential provider returned an empty token");
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

/*
 * Transport, JSON, and authentication
 */

namespace detail {

// Cloud identifiers and JSON are ASCII grammars. Unlike <cctype>, these
// helpers are independent of the process-wide C locale and accept plain char.
inline bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_ascii_lower(char c) { return c >= 'a' && c <= 'z'; }
inline bool is_ascii_upper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool is_ascii_alpha(char c) { return is_ascii_lower(c) || is_ascii_upper(c); }
inline bool is_ascii_alnum(char c) { return is_ascii_alpha(c) || is_ascii_digit(c); }
inline bool is_ascii_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
inline bool is_json_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
inline char ascii_lower(char c) {
  return is_ascii_upper(c) ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline std::string trim(std::string value) {
  const auto not_space = [](char c) { return !is_ascii_space(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

inline std::string env(std::string_view name) {
  const std::string key(name);
  if (const char* value = std::getenv(key.c_str())) return value;
  return {};
}

inline std::string header(std::string_view name, std::string_view value) {
  if (value.find('\r') != std::string_view::npos || value.find('\n') != std::string_view::npos)
    throw Error("Invalid newline in HTTP header value");
  return std::string(name) + ": " + std::string(value);
}

inline std::string base_url(std::string value) {
  while (!value.empty() && value.back() == '/') value.pop_back();
  return value;
}

inline std::string encode(std::string_view input) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(input.size() + input.size() / 4);
  for (const char raw : input) {
    const auto c = static_cast<unsigned char>(raw);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0f]);
    }
  }
  return out;
}

inline std::string json_quote(std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const char raw : value) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 0x0f]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

class Json {
 public:
  struct Number { std::string text; };
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json, std::less<>>;
  using Value = std::variant<std::nullptr_t, bool, std::string, Number, Array, Object>;

  Json() : value_(nullptr) {}
  explicit Json(Value value) : value_(std::move(value)) {}

  [[nodiscard]] bool is_object() const { return std::holds_alternative<Object>(value_); }

  [[nodiscard]] const Array& array() const {
    static const Array empty;
    if (const auto* p = std::get_if<Array>(&value_)) return *p;
    return empty;
  }

  [[nodiscard]] const Json* get(std::string_view key) const {
    const auto* obj = std::get_if<Object>(&value_);
    if (!obj) return nullptr;
    const auto it = obj->find(key);
    return it == obj->end() ? nullptr : &it->second;
  }

  [[nodiscard]] std::string text(std::string fallback = {}) const {
    if (const auto* p = std::get_if<std::string>(&value_)) return *p;
    if (const auto* p = std::get_if<Number>(&value_)) return p->text;
    return fallback;
  }

  [[nodiscard]] bool boolean(bool fallback = false) const {
    if (const auto* p = std::get_if<bool>(&value_)) return *p;
    return fallback;
  }

 private:
  Value value_;
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  Json parse() {
    whitespace();
    Json value = parse_value();
    whitespace();
    if (position_ != input_.size()) fail("trailing characters");
    return value;
  }

 private:
  [[noreturn]] void fail(std::string_view why) const {
    throw Error("Invalid JSON at byte " + std::to_string(position_) + ": " + std::string(why));
  }

  void whitespace() {
    while (position_ < input_.size() && is_json_space(input_[position_])) ++position_;
  }

  char take() {
    if (position_ >= input_.size()) fail("unexpected end");
    return input_[position_++];
  }

  bool consume(std::string_view token) {
    if (input_.substr(position_, token.size()) != token) return false;
    position_ += token.size();
    return true;
  }

  Json parse_value() {
    whitespace();
    if (position_ >= input_.size()) fail("expected value");
    switch (input_[position_]) {
      case 'n':
        if (consume("null")) return Json(nullptr);
        break;
      case 't':
        if (consume("true")) return Json(true);
        break;
      case 'f':
        if (consume("false")) return Json(false);
        break;
      case '"': return Json(parse_string());
      case '[': return parse_array();
      case '{': return parse_object();
      default:
        if (input_[position_] == '-' || is_ascii_digit(input_[position_])) return parse_number();
    }
    fail("expected value");
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7f) {
      out.push_back(static_cast<char>(cp));
      return;
    }
    static constexpr unsigned char lead[] = {0, 0, 0xc0, 0xe0, 0xf0};
    const int bytes = cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;
    out.push_back(static_cast<char>(lead[bytes] | (cp >> (6 * (bytes - 1)))));
    for (int shift = 6 * (bytes - 2); shift >= 0; shift -= 6)
      out.push_back(static_cast<char>(0x80 | ((cp >> shift) & 0x3f)));
  }

  std::uint32_t hex4() {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = take();
      value <<= 4;
      if (c >= '0' && c <= '9')
        value |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f')
        value |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        value |= static_cast<unsigned>(c - 'A' + 10);
      else
        fail("invalid unicode escape");
    }
    return value;
  }

  std::string parse_string() {
    if (take() != '"') fail("expected string");
    std::string out;
    while (true) {
      const unsigned char c = static_cast<unsigned char>(take());
      if (c == '"') return out;
      if (c < 0x20) fail("control character in string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      switch (take()) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t cp = hex4();
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (take() != '\\' || take() != 'u') fail("invalid surrogate pair");
            const std::uint32_t low = hex4();
            if (low < 0xdc00 || low > 0xdfff) fail("invalid surrogate pair");
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            fail("unpaired low surrogate");
          }
          append_utf8(out, cp);
          break;
        }
        default: fail("invalid string escape");
      }
    }
  }

  Json parse_number() {
    const std::size_t begin = position_;
    if (input_[position_] == '-') ++position_;
    if (position_ >= input_.size()) fail("invalid number");
    if (input_[position_] == '0') {
      ++position_;
    } else {
      digits("invalid number");
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      digits("invalid number");
    }
    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-'))
        ++position_;
      digits("invalid exponent");
    }
    return Json(Json::Number{std::string(input_.substr(begin, position_ - begin))});
  }

  void digits(std::string_view error) {
    if (position_ >= input_.size() || !is_ascii_digit(input_[position_])) fail(error);
    while (position_ < input_.size() && is_ascii_digit(input_[position_])) ++position_;
  }

  template <typename Collection, typename Append>
  Json parse_collection(char close, std::string_view separator_error, Collection out,
                        Append append) {
    take();
    whitespace();
    if (position_ < input_.size() && input_[position_] == close) {
      ++position_;
      return Json(std::move(out));
    }
    while (true) {
      append(out);
      whitespace();
      const char separator = take();
      if (separator == close) return Json(std::move(out));
      if (separator != ',') fail(separator_error);
      whitespace();
    }
  }

  Json parse_array() {
    return parse_collection(']', "expected ',' or ']'", Json::Array{},
                            [&](auto& out) { out.push_back(parse_value()); });
  }

  Json parse_object() {
    return parse_collection('}', "expected ',' or '}'", Json::Object{}, [&](auto& out) {
      if (position_ >= input_.size() || input_[position_] != '"') fail("expected object key");
      std::string key = parse_string();
      whitespace();
      if (take() != ':') fail("expected ':'");
      whitespace();
      out.emplace(std::move(key), parse_value());
    });
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

inline Json parse_json(std::string_view text) { return JsonParser(text).parse(); }

inline std::string field(const Json& value, std::string_view key) {
  const Json* child = value.get(key);
  return child ? child->text() : std::string{};
}

template <typename Function>
inline void for_each_json(const Json& value, std::string_view key, Function function) {
  if (const auto* items = value.get(key))
    for (const auto& item : items->array()) function(item);
}

template <typename Item, typename Fetch, typename Parse, typename Visit>
inline std::vector<Item> paginate(std::size_t limit, Fetch fetch, Parse parse, Visit visit) {
  std::vector<Item> result;
  std::string page;
  do {
    const Json json = fetch(page);
    for_each_json(json, "items", [&](const Json& item) {
      if (!limit || result.size() < limit) result.push_back(parse(item));
    });
    visit(json);
    page = limit && result.size() >= limit ? std::string{} : field(json, "nextPageToken");
  } while (!page.empty());
  return result;
}

inline std::uint64_t unsigned_field(const Json& value, std::string_view key) {
  const std::string text = field(value, key);
  if (text.empty()) return 0;
  try {
    return std::stoull(text);
  } catch (...) {
    throw Error("Invalid unsigned integer in JSON field '" + std::string(key) + "'");
  }
}

inline std::string last_path_segment(std::string value) {
  const auto slash = value.find_last_of('/');
  return slash == std::string::npos ? value : value.substr(slash + 1);
}

inline std::uint32_t crc32c_update(std::uint32_t crc, const unsigned char* data, std::size_t size) {
  crc = ~crc;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
  }
  return ~crc;
}

inline std::string crc32c_base64(std::uint32_t crc) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const std::array<unsigned char, 4> bytes{
      static_cast<unsigned char>(crc >> 24), static_cast<unsigned char>(crc >> 16),
      static_cast<unsigned char>(crc >> 8), static_cast<unsigned char>(crc)};
  std::string out;
  out.reserve(8);
  out.push_back(alphabet[bytes[0] >> 2]);
  out.push_back(alphabet[((bytes[0] & 3) << 4) | (bytes[1] >> 4)]);
  out.push_back(alphabet[((bytes[1] & 15) << 2) | (bytes[2] >> 6)]);
  out.push_back(alphabet[bytes[2] & 63]);
  out.push_back(alphabet[bytes[3] >> 2]);
  out.push_back(alphabet[(bytes[3] & 3) << 4]);
  out += "==";
  return out;
}

inline std::string crc32c(std::string_view data) {
  return crc32c_base64(
      crc32c_update(0, reinterpret_cast<const unsigned char*>(data.data()), data.size()));
}

struct UploadSource {
  std::filesystem::path path;
  std::shared_ptr<std::ifstream> stream;
  curl_off_t size = 0;
  std::string crc32c;
};

inline std::shared_ptr<UploadSource> prepare_upload(const std::filesystem::path& path,
                                                    bool checksum) {
  auto stream = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!*stream) throw Error("Cannot open upload file: " + path.string());
  stream->seekg(0, std::ios::end);
  const std::streampos end = stream->tellg();
  if (end == std::streampos(-1)) throw Error("Cannot determine upload file size: " + path.string());
  const std::streamoff length = end;
  if (length < 0 || static_cast<std::uintmax_t>(length) >
                        static_cast<std::uintmax_t>(std::numeric_limits<curl_off_t>::max()))
    throw Error("Upload file is too large for libcurl: " + path.string());
  stream->seekg(0, std::ios::beg);
  if (!*stream) throw Error("Cannot seek upload file: " + path.string());

  std::string digest;
  if (checksum) {
    std::array<char, 64 * 1024> buffer{};
    std::uint32_t crc = 0;
    while (stream->read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           stream->gcount() > 0) {
      const auto count = stream->gcount();
      crc = crc32c_update(crc, reinterpret_cast<const unsigned char*>(buffer.data()),
                          static_cast<std::size_t>(count));
    }
    if (stream->bad()) throw Error("Failed while checksumming upload file: " + path.string());
    digest = crc32c_base64(crc);
    stream->clear();
    stream->seekg(0, std::ios::beg);
    if (!*stream) throw Error("Cannot rewind upload file: " + path.string());
  }
  return std::make_shared<UploadSource>(
      UploadSource{path, std::move(stream), static_cast<curl_off_t>(length), std::move(digest)});
}

inline std::string random_uuid() {
  thread_local std::mt19937_64 random = [] {
    std::random_device source;
    std::array<std::uint32_t, 8> seed{};
    for (auto& word : seed) word = source();
    std::seed_seq sequence(seed.begin(), seed.end());
    return std::mt19937_64(sequence);
  }();
  std::uniform_int_distribution<unsigned> byte(0, 255);
  std::array<unsigned char, 16> data{};
  for (auto& value : data) value = static_cast<unsigned char>(byte(random));
  data[6] = static_cast<unsigned char>((data[6] & 0x0f) | 0x40);
  data[8] = static_cast<unsigned char>((data[8] & 0x3f) | 0x80);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(data[i]);
  }
  return out.str();
}

struct CurlGlobal {
  CurlGlobal() {
    const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) throw Error("curl_global_init failed");
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !(info->features & CURL_VERSION_THREADSAFE))
      throw Error("cloud.hpp requires a thread-safe libcurl build");
  }
  // Deliberately process-lifetime: destructor-time cleanup can race clients
  // owned by other static objects.
  ~CurlGlobal() = default;
};

static_assert(LIBCURL_VERSION_NUM >= 0x075400, "cloud.hpp requires libcurl 7.84.0 or newer");

inline void ensure_curl() { static CurlGlobal global; (void)global; }

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<std::string> headers;
  std::string body;
  std::shared_ptr<UploadSource> upload_file;
  std::optional<std::filesystem::path> download_file;
  std::optional<std::string> expected_crc32c;
  bool calculate_crc32c = false;
  bool accept_json = true;
  bool no_proxy = false;
  std::optional<std::chrono::milliseconds> timeout;
  std::chrono::milliseconds connect_timeout{10'000};
};

struct HttpResponse {
  long status = 0;
  std::string body;
  std::string crc32c;
};

struct WriteSink {
  std::string* text = nullptr;
  std::ofstream* file = nullptr;
  bool checksum = false;
  std::uint32_t crc = 0;
  bool io_error = false;
  std::exception_ptr exception;
};

inline std::size_t write_callback(char* data, std::size_t size, std::size_t count,
                                  void* userdata) noexcept {
  auto& sink = *static_cast<WriteSink*>(userdata);
  try {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
      sink.io_error = true;
      return 0;
    }
    const std::size_t bytes = size * count;
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
    if (source.remaining <= 0) return 0;
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

template <typename Value>
inline void curl_set(CURL* curl, CURLoption option, Value value) {
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
  if (!input) return {};
  std::string result(max_bytes, '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  result.resize(static_cast<std::size_t>(input.gcount()));
  return result;
}

inline void remove_noexcept(const std::filesystem::path& path) noexcept {
  std::error_code ignored;
  (void)std::filesystem::remove(path, ignored);
}

class TemporaryPathGuard {
 public:
  TemporaryPathGuard() = default;
  explicit TemporaryPathGuard(std::filesystem::path path) : path_(std::move(path)), active_(true) {}
  TemporaryPathGuard(const TemporaryPathGuard&) = delete;
  TemporaryPathGuard& operator=(const TemporaryPathGuard&) = delete;
  ~TemporaryPathGuard() {
    if (active_) remove_noexcept(path_);
  }
  void dismiss() noexcept { active_ = false; }

 private:
  std::filesystem::path path_;
  bool active_ = false;
};

inline HttpResponse http(HttpRequest request) {
  ensure_curl();
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
  if (!curl) throw Error("curl_easy_init failed");

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
  curl_set(curl.get(), CURLOPT_USERAGENT, "cloud.hpp/" CLOUD_HPP_VERSION);
  if (request.no_proxy) curl_set(curl.get(), CURLOPT_NOPROXY, "*");

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
  if (headers) curl_set(curl.get(), CURLOPT_HTTPHEADER, headers.get());

  ReadSource upload_source;
  if (request.upload_file) {
    auto& upload = *request.upload_file->stream;
    upload.clear();
    upload.seekg(0, std::ios::beg);
    if (!upload) throw Error("Cannot rewind upload file: " + request.upload_file->path.string());
    upload_source.stream = &upload;
    upload_source.remaining = request.upload_file->size;
    curl_set(curl.get(), CURLOPT_POST, 1L);
    curl_set(curl.get(), CURLOPT_READFUNCTION, &read_callback);
    curl_set(curl.get(), CURLOPT_READDATA, &upload_source);
    curl_set(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, request.upload_file->size);
  } else if (request.method == "POST") {
    curl_set(curl.get(), CURLOPT_POST, 1L);
    curl_set(curl.get(), CURLOPT_POSTFIELDS, request.body.data());
    curl_set(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, curl_size(request.body.size()));
  } else if (request.method != "GET") {
    curl_set(curl.get(), CURLOPT_CUSTOMREQUEST, request.method.c_str());
    if (!request.body.empty()) {
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
    if (!parent.empty()) std::filesystem::create_directories(parent);
    temporary = *request.download_file;
    temporary += ".gcp-part-" + random_uuid();
    temporary_guard.emplace(temporary);
    output.open(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw Error("Cannot open download destination: " + temporary.string());
  }
  WriteSink sink{request.download_file ? nullptr : &response.body,
                 request.download_file ? &output : nullptr,
                 request.calculate_crc32c || request.expected_crc32c.has_value(), 0};
  curl_set(curl.get(), CURLOPT_WRITEFUNCTION, &write_callback);
  curl_set(curl.get(), CURLOPT_WRITEDATA, &sink);

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
  if (sink.checksum) response.crc32c = crc32c_base64(sink.crc);

  const auto callback_error = [&]() -> std::optional<std::string> {
    if (sink.exception) return "HTTP response callback threw an exception";
    if (sink.io_error) return "HTTP response could not be written";
    if (upload_source.exception) return "Upload callback threw an exception";
    if (upload_source.io_error) return "Upload file could not be read";
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
        backup += ".gcp-backup-" + random_uuid();
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

inline std::string metadata_host() {
  std::string host = env("GCE_METADATA_HOST");
  if (host.empty()) host = "metadata.google.internal";
  if (host.starts_with("http://") || host.starts_with("https://")) return base_url(host);
  return "http://" + host;
}

inline std::optional<std::string> metadata_get(
    std::string_view path, std::chrono::milliseconds timeout = std::chrono::milliseconds(800)) {
  try {
    auto response = http(HttpRequest{
        .method = "GET",
        .url = metadata_host() + "/computeMetadata/v1/" + std::string(path),
        .headers = {"Metadata-Flavor: Google"},
        .no_proxy = true,
        .timeout = timeout,
        .connect_timeout = std::min(timeout, std::chrono::milliseconds(300)),
    });
    if (response.status == 200) return trim(std::move(response.body));
  } catch (...) {
  }
  return std::nullopt;
}

inline AccessToken metadata_token() {
  for (int attempt = 0; attempt < 3; ++attempt) {
    try {
      auto response = http(HttpRequest{
          .method = "GET",
          .url = metadata_host() + "/computeMetadata/v1/instance/service-accounts/default/token",
          .headers = {"Metadata-Flavor: Google"},
          .no_proxy = true,
          .timeout = std::chrono::milliseconds(1500),
          .connect_timeout = std::chrono::milliseconds(300),
      });
      if (response.status == 200) {
        const Json json = parse_json(response.body);
        const std::string token = field(json, "access_token");
        const auto seconds = unsigned_field(json, "expires_in");
        if (token.empty() || seconds == 0) throw Error("Malformed metadata token response");
        return AccessToken{
            token, std::chrono::system_clock::now() + std::chrono::seconds(seconds), {}};
      }
      if (response.status != 429 && response.status < 500)
        throw Error("Metadata token request failed", response.status, response.body);
    } catch (const Error& error) {
      if (error.http_status() > 0 && error.http_status() != 429 && error.http_status() < 500) throw;
      if (attempt == 2) throw;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
  }
  throw Error("GCP metadata token is unavailable");
}

inline AccessToken refresh_authorized_user(const Json& json) {
  const std::string client_id = field(json, "client_id");
  const std::string client_secret = field(json, "client_secret");
  const std::string refresh_token = field(json, "refresh_token");
  if (client_id.empty() || client_secret.empty() || refresh_token.empty())
    throw Error("Malformed authorized_user ADC file");
  const std::string body = "grant_type=refresh_token&client_id=" + encode(client_id) +
                           "&client_secret=" + encode(client_secret) +
                           "&refresh_token=" + encode(refresh_token);
  auto response = http(HttpRequest{
      .method = "POST",
      .url = "https://oauth2.googleapis.com/token",
      .headers = {"Content-Type: application/x-www-form-urlencoded"},
      .body = body,
      .timeout = std::chrono::milliseconds(30'000),
  });
  if (response.status < 200 || response.status >= 300)
    throw Error("OAuth token refresh failed", response.status, response.body);
  const Json token_json = parse_json(response.body);
  const std::string token = field(token_json, "access_token");
  const auto seconds = unsigned_field(token_json, "expires_in");
  if (token.empty() || seconds == 0) throw Error("Malformed OAuth token response");
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

inline Json read_json_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw Error("Cannot open credential file: " + path.string());
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (!input.good() && !input.eof()) throw Error("Cannot read credential file: " + path.string());
  return parse_json(buffer.str());
}

inline AccessToken token_from_adc_file(const std::filesystem::path& path) {
  const Json json = read_json_file(path);
  const std::string type = field(json, "type");
  if (type == "authorized_user") return refresh_authorized_user(json);
  throw Error("Unsupported ADC credential type '" + type +
              "'; inject a token callback for this credential type");
}

inline AccessToken automatic_token() {
  if (std::string token = env("GCP_ACCESS_TOKEN"); !token.empty())
    return AccessToken{std::move(token), std::chrono::system_clock::time_point::max(), {}};
  if (std::string token = env("GOOGLE_OAUTH_ACCESS_TOKEN"); !token.empty())
    return AccessToken{std::move(token), std::chrono::system_clock::time_point::max(), {}};

  if (const std::string explicit_path = env("GOOGLE_APPLICATION_CREDENTIALS");
      !explicit_path.empty())
    return token_from_adc_file(explicit_path);

  if (const auto path = well_known_adc_path(); path && std::filesystem::exists(*path))
    return token_from_adc_file(*path);

  try {
    return metadata_token();
  } catch (const Error&) {
  }

  throw Error(
      "No usable Google credentials; use ADC, an attached service account, "
      "or Credentials::from(...)");
}

inline std::string compact_error_body(std::string body) {
  constexpr std::size_t max = 8 * 1024;
  if (body.size() > max) body.resize(max);
  return trim(std::move(body));
}

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
    if (!resolved_project.empty()) return resolved_project;
    resolved_project = config.project;
    if (resolved_project.empty()) resolved_project = env("GOOGLE_CLOUD_PROJECT");
    if (resolved_project.empty()) resolved_project = env("GCLOUD_PROJECT");
    if (resolved_project.empty()) resolved_project = env("GCP_PROJECT");
    if (resolved_project.empty()) {
      if (const auto value = metadata_get("project/project-id")) resolved_project = *value;
    }
    if (resolved_project.empty())
      throw Error("No GCP project configured; set Config::project or GOOGLE_CLOUD_PROJECT");
    return resolved_project;
  }

  [[nodiscard]] std::string zone() const {
    std::lock_guard lock(discovery_mutex);
    if (!resolved_zone.empty()) return resolved_zone;
    resolved_zone = config.zone;
    if (resolved_zone.empty()) resolved_zone = env("GOOGLE_CLOUD_ZONE");
    if (resolved_zone.empty()) resolved_zone = env("GCP_ZONE");
    if (resolved_zone.empty()) {
      if (const auto value = metadata_get("instance/zone"))
        resolved_zone = last_path_segment(*value);
    }
    if (resolved_zone.empty())
      throw Error("No Compute Engine zone configured; set Config::zone or GOOGLE_CLOUD_ZONE");
    return resolved_zone;
  }

  HttpResponse call(HttpRequest request) const {
    if (!request.timeout) request.timeout = config.timeout;

    const auto execute = [this](HttpRequest current) {
      const AccessToken token = config.credentials.access_token();
      current.headers.push_back(header("Authorization", "Bearer " + token.value));
      if (current.accept_json) current.headers.push_back("Accept: application/json");
      std::string quota = config.quota_project;
      if (quota.empty()) quota = env("GOOGLE_CLOUD_QUOTA_PROJECT");
      if (quota.empty()) quota = token.quota_project;
      if (!quota.empty()) current.headers.push_back(header("X-Goog-User-Project", quota));
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
    if (endpoint.starts_with("https://")) return;
    if (endpoint.starts_with("http://") && config.allow_insecure_http) return;
    throw Error("Config::" + std::string(option_name) +
                " must use HTTPS (or set allow_insecure_http explicitly)");
  }

  static void check(const HttpResponse& response) {
    if (response.status >= 200 && response.status < 300) return;
    const std::string body = compact_error_body(response.body);
    std::string message =
        "Google Cloud request failed with HTTP " + std::to_string(response.status);
    if (!body.empty()) message += ": " + body;
    throw Error(std::move(message), response.status, body);
  }

  mutable std::mutex discovery_mutex;
  mutable std::string resolved_project;
  mutable std::string resolved_zone;
};

inline Object parse_object(const Json& json) {
  return Object{
      .name = field(json, "name"),
      .generation = field(json, "generation"),
      .size = unsigned_field(json, "size"),
      .content_type = field(json, "contentType"),
      .updated = field(json, "updated"),
      .etag = field(json, "etag"),
      .crc32c = field(json, "crc32c"),
      .md5_hash = field(json, "md5Hash"),
  };
}

inline Instance parse_instance(const Json& json) {
  Instance result{
      .id = field(json, "id"),
      .name = field(json, "name"),
      .zone = last_path_segment(field(json, "zone")),
      .machine_type = last_path_segment(field(json, "machineType")),
      .status = field(json, "status"),
      .creation_timestamp = field(json, "creationTimestamp"),
  };
  for_each_json(json, "networkInterfaces", [&](const Json& interface) {
    if (result.internal_ip.empty()) result.internal_ip = field(interface, "networkIP");
    for_each_json(interface, "accessConfigs", [&](const Json& config) {
      if (result.external_ip.empty()) result.external_ip = field(config, "natIP");
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
      if (message.empty()) message = field(item, "code");
      if (!message.empty()) {
        if (!result.empty()) result += "; ";
        result += message;
      }
    });
  if (result.empty()) result = field(json, "httpErrorMessage");
  const std::string status_code = field(json, "httpErrorStatusCode");
  if (result.empty() && !status_code.empty() && status_code != "0")
    result = "HTTP status " + status_code;
  if (result.empty() && error && error->is_object()) result = "unknown operation error";
  return result;
}

}  // namespace detail

/*
 * Storage and Compute primitives
 */

class Bucket {
 public:
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

  [[nodiscard]] Object stat(std::string_view object) const {
    return detail::parse_object(core_->json(detail::HttpRequest{
        .url = storage(
            "/storage/v1/b/" + detail::encode(name_) + "/o/" + detail::encode(object) +
            "?fields=name%2Cgeneration%2Csize%2CcontentType%2Cupdated%2Cetag%2Ccrc32c%2Cmd5Hash"),
    }));
  }

  [[nodiscard]] ObjectList list(ListOptions options = {}) const {
    ObjectList result;
    result.objects = detail::paginate<Object>(
        options.limit,
        [&](const std::string& page) {
          std::string query = "?maxResults=1000";
          if (!options.prefix.empty()) query += "&prefix=" + detail::encode(options.prefix);
          if (!options.delimiter.empty())
            query += "&delimiter=" + detail::encode(options.delimiter);
          if (options.versions) query += "&versions=true";
          if (!page.empty()) query += "&pageToken=" + detail::encode(page);
          return core_->json(detail::HttpRequest{
              .url = storage("/storage/v1/b/" + detail::encode(name_) + "/o" + query),
          });
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
    Object result = detail::parse_object(core_->json(detail::HttpRequest{
        .method = "POST",
        .url = upload_url(object, options),
        .headers = upload_headers(options, checksum),
        .body = std::string(bytes),
    }));
    if (!checksum.empty() && checksum != result.crc32c)
      throw Error("Cloud Storage upload checksum mismatch");
    return result;
  }

  [[nodiscard]] Object put_file(std::string_view object, const std::filesystem::path& source,
                                PutOptions options = {}) const {
    const auto upload = detail::prepare_upload(source, options.crc32c);
    const std::string& checksum = upload->crc32c;
    Object result = detail::parse_object(core_->json(detail::HttpRequest{
        .method = "POST",
        .url = upload_url(object, options),
        .headers = upload_headers(options, checksum),
        .upload_file = upload,
        .timeout = core_->config.transfer_timeout,
    }));
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
      request.expected_crc32c = verify ? std::optional<std::string>(metadata.crc32c) : std::nullopt;
      request.timeout = core_->config.transfer_timeout;
    });
  }

  void erase(std::string_view object, std::optional<std::string> generation = std::nullopt) const {
    std::string query;
    if (generation) {
      query = "?generation=" + detail::encode(*generation) +
              "&ifGenerationMatch=" + detail::encode(*generation);
    }
    core_->call(detail::HttpRequest{
        .method = "DELETE",
        .url = storage("/storage/v1/b/" + detail::encode(name_) + "/o/" + detail::encode(object) +
                       query),
    });
  }

 private:
  friend class Cloud;
  Bucket(std::shared_ptr<detail::Core> core, std::string name)
      : core_(std::move(core)), name_(std::move(name)) {
    if (name_.empty()) throw Error("Bucket name must not be empty");
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
    if (!checksum.empty()) headers.push_back(detail::header("X-Goog-Hash", "crc32c=" + checksum));
    return headers;
  }

  template <typename Configure>
  [[nodiscard]] detail::HttpResponse download(std::string_view object, bool verify,
                                              Configure configure) const {
    Object metadata;
    if (verify) metadata = stat(object);
    require_verification_metadata(metadata, verify);
    std::string query = "?alt=media";
    if (verify) query += "&generation=" + detail::encode(metadata.generation);
    detail::HttpRequest request{
        .url = storage("/download/storage/v1/b/" + detail::encode(name_) + "/o/" +
                       detail::encode(object) + query),
        .headers = {"Accept-Encoding: gzip"},
        .calculate_crc32c = verify,
        .accept_json = false,
    };
    configure(request, metadata);
    auto response = core_->call(std::move(request));
    if (verify && response.crc32c != metadata.crc32c)
      throw Error("Cloud Storage download checksum mismatch");
    return response;
  }

  static void require_verification_metadata(const Object& metadata, bool verify) {
    if (!verify) return;
    if (metadata.generation.empty() || metadata.crc32c.empty())
      throw Error(
          "Cloud Storage object lacks generation/CRC32C metadata; "
          "verified download is unavailable");
  }

  std::shared_ptr<detail::Core> core_;
  std::string name_;
};

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
      if (now >= deadline) throw Error("Timed out waiting for Compute operation '" + name_ + "'");
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const detail::Json json = core_->json(detail::HttpRequest{
          .url = core_->compute_url(zone_, "/operations/" + detail::encode(name_)),
          .timeout =
              std::max(std::chrono::milliseconds(1), std::min(core_->config.timeout, remaining)),
      });
      if (detail::field(json, "status") == "DONE") {
        const std::string failure = detail::operation_error(json);
        if (!failure.empty()) throw Error("Compute operation failed: " + failure);
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
    if (name_.empty()) throw Error("Malformed Compute operation response: missing name");
  }

  std::shared_ptr<detail::Core> core_;
  std::string name_;
  std::string zone_;
};

class Vm {
 public:
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

  [[nodiscard]] Instance get() const {
    return detail::parse_instance(core_->json(detail::HttpRequest{.url = instance_url()}));
  }

  [[nodiscard]] std::string status() const { return get().status; }

  [[nodiscard]] Operation start() const { return action("start"); }
  [[nodiscard]] Operation stop(bool discard_local_ssd = false) const {
    return action(std::string("stop?discardLocalSsd=") + (discard_local_ssd ? "true" : "false") +
                      "&requestId=" + detail::random_uuid(),
                  false);
  }
  [[nodiscard]] Operation erase() const { return action("", true); }

 private:
  friend class Cloud;
  Vm(std::shared_ptr<detail::Core> core, std::string name)
      : core_(std::move(core)), name_(std::move(name)) {
    if (name_.empty()) throw Error("VM name must not be empty");
  }

  [[nodiscard]] std::string instance_url() const {
    return core_->compute_url(core_->zone(), "/instances/" + detail::encode(name_));
  }

  [[nodiscard]] Operation action(std::string action, bool deleting = false) const {
    std::string url = instance_url();
    if (deleting) {
      url += "?requestId=" + detail::random_uuid();
    } else if (action.starts_with("stop?")) {
      url += "/" + action;
    } else {
      url += "/" + action + "?requestId=" + detail::random_uuid();
    }
    const detail::Json json = core_->json(detail::HttpRequest{
        .method = deleting ? "DELETE" : "POST",
        .url = std::move(url),
    });
    return Operation(core_, detail::field(json, "name"), core_->zone());
  }

  std::shared_ptr<detail::Core> core_;
  std::string name_;
};

class Cloud {
 public:
  explicit Cloud(Config config = {}) : core_(std::make_shared<detail::Core>(std::move(config))) {}

  [[nodiscard]] Bucket bucket(std::string name) const { return Bucket(core_, std::move(name)); }

  [[nodiscard]] Vm vm(std::string name) const { return Vm(core_, std::move(name)); }

  [[nodiscard]] Operation create_from_template(std::string name,
                                               std::string instance_template) const {
    if (name.empty() || instance_template.empty())
      throw Error("VM name and instance template must not be empty");
    if (!instance_template.starts_with("projects/") && !instance_template.starts_with("global/"))
      instance_template = "global/instanceTemplates/" + instance_template;
    const std::string request_id = detail::random_uuid();
    const detail::Json json = core_->json(detail::HttpRequest{
        .method = "POST",
        .url = core_->compute_url(core_->zone(), "/instances?sourceInstanceTemplate=" +
                                                    detail::encode(instance_template) +
                                                    "&requestId=" + request_id),
        .headers = {"Content-Type: application/json"},
        .body = "{\"name\":" + detail::json_quote(name) + "}",
    });
    return Operation(core_, detail::field(json, "name"), core_->zone());
  }

  [[nodiscard]] std::vector<Instance> vms(std::size_t limit = 0) const {
    return detail::paginate<Instance>(
        limit,
        [&](const std::string& page) {
          std::string query = "?maxResults=500";
          if (!page.empty()) query += "&pageToken=" + detail::encode(page);
          return core_->json(detail::HttpRequest{
              .url = core_->compute_url(core_->zone(), "/instances" + query),
          });
        },
        detail::parse_instance, [](const detail::Json&) {});
  }

  [[nodiscard]] std::string project() const { return core_->project(); }
  [[nodiscard]] std::string zone() const { return core_->zone(); }

 private:
  friend struct ::cloud::detail::client_state;
  std::shared_ptr<detail::Core> core_;
};

}  // namespace cloud::gcp

/*
 * Provider-independent API
 */

namespace cloud {

using error = gcp::Error;
using access_token = gcp::AccessToken;
using token_provider = std::function<access_token()>;
using object = gcp::Object;
using object_list = gcp::ObjectList;
using list_options = gcp::ListOptions;
using put_options = gcp::PutOptions;
using instance = gcp::Instance;
using operation = gcp::Operation;

using provider = std::string;
enum class selection { ordered, lowest_cost };
enum class feature {
  object_storage, containers, spot_instances, storage_mounts,
  log_streaming, raw_instances, accelerators, cost_estimates,
};

class auth {
 public:
  auth() : credentials_(gcp::Credentials::automatic()) {}
  static auth default_chain() { return {}; }
  static auth bearer(std::string token) { return auth(gcp::Credentials::bearer(std::move(token))); }
  static auth from(token_provider callback) {
    return auth(gcp::Credentials::from(std::move(callback)));
  }

 private:
  friend struct detail::client_state;
  explicit auth(gcp::Credentials credentials) : credentials_(std::move(credentials)) {}
  gcp::Credentials credentials_;
};

struct resources {
  unsigned cpus = 1;
  double memory_gb = 1;
  std::string gpu;
  bool spot = false;
  std::optional<double> max_price_per_hour;
};

struct mount {
  std::string source;
  std::string target;
  bool read_only = false;
};

struct job_spec {
  // command is direct argv and mount sources are buckets or slash-terminated
  // prefixes. timeout is both the controller deadline and Batch's per-attempt cap.
  std::string name = "job";
  std::string image;
  std::vector<std::string> command;
  std::string workdir;
  std::string service_account;
  std::vector<mount> mounts;
  cloud::resources resources;
  unsigned retries = 0;
  bool auto_delete = true;
  std::chrono::milliseconds timeout{std::chrono::hours(1)};
};

struct plan {
  // Estimates are advisory. A maximum price fails closed unless the caller
  // supplied an estimate for the selected provider and machine.
  cloud::provider provider = "gcp";
  std::string region;
  std::string machine_type;
  std::optional<double> estimated_hourly_cost;
  std::optional<double> estimated_egress_cost;
  std::vector<std::string> warnings;
};

#define CLOUD_HPP_JOB_STATES(X)             \
  X(queued, "QUEUED")                       \
  X(scheduled, "SCHEDULED")                 \
  X(running, "RUNNING")                     \
  X(succeeded, "SUCCEEDED")                 \
  X(failed, "FAILED")                       \
  X(cancelling, "CANCELLATION_IN_PROGRESS") \
  X(cancelled, "CANCELLED")                 \
  X(deleting, "DELETION_IN_PROGRESS")
#define CLOUD_HPP_JOB_STATE_ENUM(name, unused) name,
enum class job_state {
  CLOUD_HPP_JOB_STATES(CLOUD_HPP_JOB_STATE_ENUM) unknown,
};
#undef CLOUD_HPP_JOB_STATE_ENUM

struct log_entry {
  std::string timestamp;
  std::string receive_timestamp;
  std::string id;
  std::string text;
  std::string severity;
};

struct result {
  job_state state = job_state::unknown;
  std::optional<int> exit_code;
  std::string message;
  std::vector<std::string> warnings;
  [[nodiscard]] bool success() const noexcept { return state == job_state::succeeded; }
  [[nodiscard]] const std::string& error() const noexcept { return message; }
};

using price_estimator = std::function<std::optional<double>(std::string_view, std::string_view,
                                                            std::string_view, bool)>;
using log_sink = std::function<void(const log_entry&)>;

struct config {
  std::optional<cloud::provider> provider;
  std::vector<cloud::provider> providers{"gcp"};
  cloud::selection selection = cloud::selection::ordered;
  std::string project;
  std::string region = "europe";
  std::string zone;
  cloud::auth auth = cloud::auth::default_chain();
  price_estimator estimate_hourly_cost;
  std::chrono::milliseconds request_timeout{std::chrono::minutes(1)};
  std::chrono::milliseconds transfer_timeout{std::chrono::hours(1)};
  std::chrono::milliseconds poll_interval{std::chrono::seconds(2)};
  std::chrono::milliseconds final_log_delay{std::chrono::seconds(2)};
  std::chrono::milliseconds final_log_timeout{std::chrono::seconds(30)};
  std::chrono::milliseconds cleanup_timeout{std::chrono::minutes(5)};
  bool allow_insecure_http = false;
  std::string storage_endpoint = "https://storage.googleapis.com";
  std::string compute_endpoint = "https://compute.googleapis.com";
  std::string batch_endpoint = "https://batch.googleapis.com";
  std::string logging_endpoint = "https://logging.googleapis.com";
};

/*
 * Planning and Batch controller
 */

namespace detail {

struct uri {
  std::string bucket;
  std::string key;
};

inline uri parse_uri(std::string_view value, std::string_view operation = {}) {
  constexpr std::string_view prefix = "cloud://";
  if (!value.starts_with(prefix)) throw error("Cloud URI must start with cloud://");
  value.remove_prefix(prefix.size());
  const auto slash = value.find('/');
  uri out{std::string(value.substr(0, slash)),
          slash == std::string_view::npos ? std::string{} : std::string(value.substr(slash + 1))};
  if (out.bucket.empty()) throw error("Cloud URI must contain a bucket");
  if (!operation.empty() && out.key.empty())
    throw error(std::string(operation) + "() requires an object path");
  return out;
}

inline std::string region(std::string value) {
  if (value == "europe") return "europe-west4";
  if (value == "us") return "us-central1";
  if (value == "asia") return "asia-east1";
  if (value.empty() || !gcp::detail::is_ascii_alpha(value.front()) ||
      !gcp::detail::is_ascii_alnum(value.back()))
    throw error("Invalid GCP region");
  for (const char c : value)
    if (!gcp::detail::is_ascii_lower(c) && !gcp::detail::is_ascii_digit(c) && c != '-')
      throw error("Invalid GCP region");
  return value;
}

inline void validate_project(std::string_view value) {
  if (value.empty()) throw error("GCP project must not be empty");
  for (const char c : value)
    if (!gcp::detail::is_ascii_alnum(c) && c != '-' && c != '.' && c != ':')
      throw error("Invalid GCP project identifier");
}

inline provider selected_provider(const config& cfg, std::vector<std::string>* warnings = nullptr) {
  const std::vector<provider> choices =
      cfg.provider ? std::vector<provider>{*cfg.provider} : cfg.providers;
  if (choices.empty()) throw error("No cloud provider configured");
  if (cfg.selection == selection::lowest_cost)
    throw error(
        "lowest_cost requires at least two implemented providers; "
        "only GCP is implemented");
  for (const auto& value : choices) {
    if (value == "gcp") return value;
    if (warnings) warnings->push_back(value + " backend is not implemented; skipped");
    if (cfg.provider) throw error(value + " backend is not implemented");
  }
  throw error("None of the configured cloud providers is implemented");
}

inline std::string machine(const resources& requested) {
  if (!requested.gpu.empty())
    throw error("gcp does not support requested accelerator \"" + requested.gpu +
                "\" in cloud.hpp v0.1");
  if (!requested.cpus || !(requested.memory_gb > 0) || !std::isfinite(requested.memory_gb))
    throw error("Resources require positive CPU and memory values");
  struct shape {
    const char* name;
    unsigned cpus;
    double memory;
  };
  static constexpr shape shapes[] = {
      {"e2-standard-2", 2, 8},    {"e2-standard-4", 4, 16},    {"e2-standard-8", 8, 32},
      {"e2-standard-16", 16, 64}, {"e2-standard-32", 32, 128},
  };
  for (const auto& shape : shapes)
    if (requested.cpus <= shape.cpus && requested.memory_gb <= shape.memory) return shape.name;
  throw error("No built-in GCP machine mapping satisfies the request");
}

inline void validate_spec(const job_spec& spec);

inline cloud::plan make_plan(const config& cfg, const job_spec& spec) {
  validate_spec(spec);
  cloud::plan out;
  out.provider = selected_provider(cfg, &out.warnings);
  out.region = detail::region(cfg.region);
  out.machine_type = machine(spec.resources);
  if (cfg.estimate_hourly_cost)
    out.estimated_hourly_cost =
        cfg.estimate_hourly_cost(out.provider, out.region, out.machine_type, spec.resources.spot);
  if (out.estimated_hourly_cost &&
      (!std::isfinite(*out.estimated_hourly_cost) || *out.estimated_hourly_cost < 0))
    throw error("Price estimator returned an invalid hourly price");
  if (!out.estimated_hourly_cost)
    out.warnings.push_back("hourly cost unavailable; estimates are never guarantees");
  if (spec.resources.max_price_per_hour) {
    if (!std::isfinite(*spec.resources.max_price_per_hour) ||
        *spec.resources.max_price_per_hour < 0)
      throw error("Maximum hourly price must be finite and nonnegative");
    if (!out.estimated_hourly_cost)
      throw error("A maximum hourly price requires a configured price estimator");
    if (*out.estimated_hourly_cost > *spec.resources.max_price_per_hour)
      throw error("Estimated hourly price exceeds the configured maximum");
  }
  out.warnings.push_back("egress cost is not estimated");
  if (spec.service_account.empty())
    out.warnings.push_back("Batch VM uses the default Compute Engine service account");
  return out;
}

inline std::string strings(const std::vector<std::string>& values, std::size_t begin = 0) {
  std::string out = "[";
  for (std::size_t i = begin; i < values.size(); ++i) {
    if (i != begin) out += ',';
    out += gcp::detail::json_quote(values[i]);
  }
  return out + ']';
}

inline void validate_workdir(std::string_view path) {
  for (const char c : path)
    if (!gcp::detail::is_ascii_alnum(c) && c != '/' && c != '.' && c != '_' && c != '-')
      throw error("Container workdir contains an unsafe character");
}

inline void validate_spec(const job_spec& spec) {
  if (spec.image.empty() || spec.command.empty() || spec.command.front().empty())
    throw error("A job requires a container image and a non-empty command");
  if (spec.retries > 10) throw error("GCP Batch allows at most 10 retries");
  if (spec.timeout <= std::chrono::milliseconds::zero() ||
      spec.timeout > std::chrono::hours(24 * 14))
    throw error("Job timeout must be positive and at most 14 days");
  validate_workdir(spec.workdir);
  if (spec.service_account.find('\r') != std::string::npos ||
      spec.service_account.find('\n') != std::string::npos)
    throw error("Service account contains an invalid newline");
  for (const auto& item : spec.mounts) {
    const uri source = parse_uri(item.source);
    if (!source.key.empty() && !source.key.ends_with('/'))
      throw error("GCS job mounts require a bucket or directory prefix ending in '/'");
    if (item.target.empty() || item.target.front() != '/' ||
        item.target.find(':') != std::string::npos)
      throw error("Mount targets must be absolute container paths without ':'");
  }
}

inline std::string batch_body(const job_spec& spec, const cloud::plan& chosen) {
  std::string container = "{\"imageUri\":" + gcp::detail::json_quote(spec.image) +
                          ",\"entrypoint\":" + gcp::detail::json_quote(spec.command.front()) +
                          ",\"commands\":" + strings(spec.command, 1);
  if (!spec.workdir.empty())
    container += ",\"options\":" + gcp::detail::json_quote("--workdir=" + spec.workdir);

  std::string task_volumes;
  std::string container_volumes;
  for (std::size_t i = 0; i < spec.mounts.size(); ++i) {
    const auto& item = spec.mounts[i];
    const uri source = parse_uri(item.source);
    const std::string host = "/mnt/disks/cloud-" + std::to_string(i);
    const std::string remote = source.bucket + (source.key.empty() ? "" : "/" + source.key);
    if (!task_volumes.empty()) {
      task_volumes += ',';
      container_volumes += ',';
    }
    task_volumes += "{\"gcs\":{\"remotePath\":" + gcp::detail::json_quote(remote) +
                    "},\"mountPath\":" + gcp::detail::json_quote(host) + '}';
    container_volumes +=
        gcp::detail::json_quote(host + ":" + item.target + (item.read_only ? ":ro" : ""));
  }
  if (!container_volumes.empty()) container += ",\"volumes\":[" + container_volumes + ']';
  container += '}';

  const auto milliseconds = spec.timeout.count();
  const auto seconds = milliseconds / 1000 + (milliseconds % 1000 != 0);
  const auto memory = static_cast<std::uint64_t>(std::ceil(spec.resources.memory_gb * 1024.0));
  std::string task =
      "{\"runnables\":[{\"container\":" + container + "}],\"computeResource\":{\"cpuMilli\":" +
      gcp::detail::json_quote(std::to_string(spec.resources.cpus * 1000ULL)) +
      ",\"memoryMib\":" + gcp::detail::json_quote(std::to_string(memory)) +
      "},\"maxRunDuration\":" + gcp::detail::json_quote(std::to_string(seconds) + "s") +
      ",\"maxRetryCount\":" + std::to_string(spec.retries);
  if (!task_volumes.empty()) task += ",\"volumes\":[" + task_volumes + ']';
  task += '}';

  const std::string service_account =
      spec.service_account.empty()
          ? std::string{}
          : ",\"serviceAccount\":{\"email\":" + gcp::detail::json_quote(spec.service_account) + '}';

  return "{\"taskGroups\":[{\"taskSpec\":" + task +
         ",\"taskCount\":\"1\",\"parallelism\":\"1\"}],"
         "\"allocationPolicy\":{\"location\":{\"allowedLocations\":[" +
         gcp::detail::json_quote("regions/" + chosen.region) +
         "]},\"instances\":[{\"policy\":{\"machineType\":" +
         gcp::detail::json_quote(chosen.machine_type) + ",\"provisioningModel\":\"" +
         std::string(spec.resources.spot ? "SPOT" : "STANDARD") +
         "\"},\"blockProjectSshKeys\":true}]" + service_account +
         "},"
         "\"logsPolicy\":{\"destination\":\"CLOUD_LOGGING\"},"
         "\"labels\":{\"cloud-hpp\":\"temporary\",\"cloud-hpp-ttl-seconds\":" +
         gcp::detail::json_quote(std::to_string(seconds)) + "}}";
}

inline std::string job_id(std::string value) {
  for (char& c : value) {
    c = gcp::detail::ascii_lower(c);
    if (!gcp::detail::is_ascii_alnum(c)) c = '-';
  }
  while (!value.empty() && value.back() == '-') value.pop_back();
  if (value.empty() || !gcp::detail::is_ascii_alpha(value.front())) value = "job-" + value;
  if (value.size() > 46) value.resize(46);
  std::string suffix = gcp::detail::random_uuid();
  suffix.erase(std::remove(suffix.begin(), suffix.end(), '-'), suffix.end());
  return value + '-' + suffix.substr(0, 16);
}

struct client_state {
  cloud::config config;
  gcp::Cloud raw;

  static gcp::Config low_level(const cloud::config& value) {
    gcp::Config out;
    out.project = value.project;
    out.zone = value.zone;
    out.credentials = value.auth.credentials_;
    out.timeout = value.request_timeout;
    out.transfer_timeout = value.transfer_timeout;
    out.allow_insecure_http = value.allow_insecure_http;
    out.storage_endpoint = value.storage_endpoint;
    out.compute_endpoint = value.compute_endpoint;
    out.batch_endpoint = value.batch_endpoint;
    out.logging_endpoint = value.logging_endpoint;
    return out;
  }

  explicit client_state(cloud::config value) : config(std::move(value)), raw(low_level(config)) {
    if (config.poll_interval <= std::chrono::milliseconds::zero() ||
        config.final_log_delay < std::chrono::milliseconds::zero() ||
        config.final_log_timeout < config.final_log_delay ||
        config.cleanup_timeout <= std::chrono::milliseconds::zero())
      throw error("Polling and cleanup durations must be valid");
  }

  [[nodiscard]] std::shared_ptr<gcp::detail::Core> core() const { return raw.core_; }
};

inline bool retryable(long status) {
  return status == 0 || status == 408 || status == 429 || (status >= 500 && status < 600);
}

inline gcp::detail::HttpResponse call(
    const client_state& client, const gcp::detail::HttpRequest& request,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
  // Status-less failures plus HTTP 408, 429, and 5xx are retried. Mutation
  // callers also use provider idempotency keys for ambiguous responses.
  const auto base_timeout = request.timeout.value_or(client.config.request_timeout);
  for (int attempt = 0;; ++attempt) {
    auto current = request;
    if (deadline != std::chrono::steady_clock::time_point::max()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) throw error("Cloud operation deadline exceeded");
      current.timeout =
          std::max(std::chrono::milliseconds(1),
                   std::min(base_timeout,
                            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    }
    try {
      return client.core()->call(std::move(current));
    } catch (const error& failure) {
      if (attempt == 3 || !retryable(failure.http_status())) throw;
    }
    auto backoff = std::chrono::milliseconds(100 * (1 << attempt));
    if (deadline != std::chrono::steady_clock::time_point::max())
      backoff = std::min(backoff, std::max(std::chrono::milliseconds::zero(),
                                           std::chrono::duration_cast<std::chrono::milliseconds>(
                                               deadline - std::chrono::steady_clock::now())));
    if (backoff <= std::chrono::milliseconds::zero())
      throw error("Cloud operation deadline exceeded");
    std::this_thread::sleep_for(backoff);
  }
}

inline void pause(const client_state& client, std::chrono::steady_clock::time_point deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining > std::chrono::milliseconds::zero())
    std::this_thread::sleep_for(std::min(client.config.poll_interval, remaining));
}

struct job_data {
  std::shared_ptr<client_state> client;
  job_spec spec;
  cloud::plan chosen;
  std::string id;
  std::string name;
  mutable std::string uid;
  mutable std::optional<result> cached;
  mutable std::vector<log_entry> log_cache;
  mutable std::unordered_set<std::string> log_ids;
  mutable std::string log_cursor;
  mutable std::string log_high_water;
  std::chrono::steady_clock::time_point submitted = std::chrono::steady_clock::now();
};

inline job_state parse_state(std::string_view state) {
#define CLOUD_HPP_PARSE_JOB_STATE(name, value) \
  if (state == value) return job_state::name;
  CLOUD_HPP_JOB_STATES(CLOUD_HPP_PARSE_JOB_STATE)
#undef CLOUD_HPP_PARSE_JOB_STATE
  return job_state::unknown;
}

#undef CLOUD_HPP_JOB_STATES

inline bool terminal(job_state state) {
  return state == job_state::succeeded || state == job_state::failed ||
         state == job_state::cancelled;
}

inline gcp::detail::Json get_job(
    const job_data& job,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
  auto response = call(*job.client,
                       gcp::detail::HttpRequest{
                           .url = job.client->core()->config.batch_endpoint + "/v1/" + job.name,
                       },
                       deadline);
  return gcp::detail::parse_json(response.body);
}

inline job_state update(const job_data& job, const gcp::detail::Json& json) {
  if (job.uid.empty()) job.uid = gcp::detail::field(json, "uid");
  const auto* status = json.get("status");
  return status ? parse_state(gcp::detail::field(*status, "state")) : job_state::unknown;
}

inline std::string status_error(const gcp::detail::Json& json) {
  const auto* status = json.get("status");
  std::string message;
  if (status)
    gcp::detail::for_each_json(*status, "statusEvents", [&](const gcp::detail::Json& event) {
      const std::string value = gcp::detail::field(event, "description");
      if (!value.empty()) message = value;
    });
  return message.empty() ? "Batch job failed" : message;
}

inline std::optional<int> task_exit_code(const job_data& job) {
  try {
    const auto deadline = std::chrono::steady_clock::now() + job.client->config.request_timeout;
    auto response = call(*job.client,
                         gcp::detail::HttpRequest{
                             .url = job.client->core()->config.batch_endpoint + "/v1/" + job.name +
                                    "/taskGroups/group0/tasks/0",
                         },
                         deadline);
    const auto json = gcp::detail::parse_json(response.body);
    const auto* status = json.get("status");
    const auto* events = status ? status->get("statusEvents") : nullptr;
    std::optional<int> code;
    if (events)
      for (const auto& event : events->array())
        if (const auto* execution = event.get("taskExecution")) {
          const std::string value = gcp::detail::field(*execution, "exitCode");
          if (!value.empty()) {
            try {
              code = std::stoi(value);
            } catch (const std::exception&) {
              return std::nullopt;
            }
          }
        }
    return code;
  } catch (const error&) {
    return std::nullopt;
  }
}

inline std::vector<log_entry> logs(
    const job_data& job,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
  if (job.uid.empty()) update(job, get_job(job, deadline));
  std::vector<log_entry> out;
  std::string page;
  // Query from the previous high-water receive time, then deduplicate by
  // stable log identity. The overlap avoids gaps when Logging exposes entries
  // with equal or delayed timestamps on different polls.
  do {
    const std::string project = job.client->core()->project();
    validate_project(project);
    const std::string filter =
        "logName = \"projects/" + project +
        "/logs/batch_task_logs\" AND labels.job_uid=" + job.uid +
        (job.log_cursor.empty() ? std::string{}
                                : " AND receiveTimestamp >= \"" + job.log_cursor + "\"");
    std::string body = "{\"resourceNames\":[" + gcp::detail::json_quote("projects/" + project) +
                       "],\"filter\":" + gcp::detail::json_quote(filter) +
                       ",\"orderBy\":\"timestamp desc\"";
    if (!page.empty()) body += ",\"pageToken\":" + gcp::detail::json_quote(page);
    body += '}';
    auto response =
        call(*job.client,
             gcp::detail::HttpRequest{
                 .method = "POST",
                 .url = job.client->core()->config.logging_endpoint + "/v2/entries:list",
                 .headers = {"Content-Type: application/json"},
                 .body = std::move(body),
             },
             deadline);
    const auto json = gcp::detail::parse_json(response.body);
    gcp::detail::for_each_json(json, "entries", [&](const gcp::detail::Json& entry) {
      log_entry line{
          .timestamp = gcp::detail::field(entry, "timestamp"),
          .receive_timestamp = gcp::detail::field(entry, "receiveTimestamp"),
          .id = gcp::detail::field(entry, "insertId"),
          .text = gcp::detail::field(entry, "textPayload"),
          .severity = gcp::detail::field(entry, "severity"),
      };
      if (line.text.empty())
        if (const auto* payload = entry.get("jsonPayload"))
          line.text = gcp::detail::field(*payload, "message");
      if (line.text.empty()) line.text = "[structured log entry]";
      if (line.id.empty()) line.id = line.timestamp + '\n' + line.text;
      out.push_back(std::move(line));
    });
    page = gcp::detail::field(json, "nextPageToken");
  } while (!page.empty());
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return std::tie(a.timestamp, a.id) < std::tie(b.timestamp, b.id);
  });
  for (const auto& line : out) {
    std::string received = line.receive_timestamp;
    if (received.size() == 20 && received.back() == 'Z')
      received.insert(19, ".000000000");
    else if (received.size() > 21 && received[19] == '.' && received.back() == 'Z' &&
             received.size() <= 29 &&
             std::all_of(received.begin() + 20, received.end() - 1,
                         [](char c) { return c >= '0' && c <= '9'; }))
      received.insert(received.size() - 1, 9 - (received.size() - 21), '0');
    if (received.empty()) continue;
    if (job.log_high_water.empty() || received > job.log_high_water) {
      job.log_cursor = job.log_high_water;
      job.log_high_water = std::move(received);
    } else if (received < job.log_high_water &&
               (job.log_cursor.empty() || received > job.log_cursor)) {
      job.log_cursor = std::move(received);
    }
  }
  return out;
}

inline std::vector<log_entry> merge_logs(
    const job_data& job,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
  std::vector<log_entry> added;
  for (auto& line : logs(job, deadline)) {
    const std::string identity = line.timestamp + '\n' + line.id;
    if (job.log_ids.insert(identity).second) {
      added.push_back(line);
      job.log_cache.push_back(std::move(line));
    }
  }
  std::sort(job.log_cache.begin(), job.log_cache.end(), [](const auto& a, const auto& b) {
    return std::tie(a.timestamp, a.id) < std::tie(b.timestamp, b.id);
  });
  return added;
}

template <typename Poll>
inline void drain_logs(const job_data& job, Poll poll) {
  const auto stop = std::chrono::steady_clock::now() + job.client->config.final_log_timeout;
  auto quiet_since = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < stop) {
    if (poll(stop)) quiet_since = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - quiet_since >= job.client->config.final_log_delay) break;
    pause(*job.client, stop);
  }
}

inline void wait_operation(const client_state& client, const std::string& name,
                           std::chrono::steady_clock::time_point deadline) {
  if (name.empty()) throw error("Malformed Batch operation: missing name");
  while (std::chrono::steady_clock::now() < deadline) {
    auto response = call(client,
                         gcp::detail::HttpRequest{
                             .url = client.core()->config.batch_endpoint + "/v1/" + name,
                         },
                         deadline);
    const auto json = gcp::detail::parse_json(response.body);
    if (const auto* done = json.get("done"); done && done->boolean()) {
      if (const auto* failure = json.get("error")) {
        std::string message = gcp::detail::field(*failure, "message");
        throw error("Batch cleanup failed" + (message.empty() ? std::string{} : ": " + message));
      }
      return;
    }
    pause(client, deadline);
  }
  throw error("Timed out waiting for Batch operation");
}

inline void delete_job(const job_data& job, std::string_view reason) {
  const auto deadline = std::chrono::steady_clock::now() + job.client->config.cleanup_timeout;
  gcp::detail::HttpResponse response;
  try {
    response = call(*job.client,
                    gcp::detail::HttpRequest{
                        .method = "DELETE",
                        .url = job.client->core()->config.batch_endpoint + "/v1/" + job.name +
                               "?reason=" + gcp::detail::encode(reason) +
                               "&requestId=" + gcp::detail::random_uuid(),
                    },
                    deadline);
  } catch (const error& failure) {
    if (failure.http_status() == 404) return;
    throw;
  }
  wait_operation(*job.client, gcp::detail::field(gcp::detail::parse_json(response.body), "name"),
                 deadline);
}

inline gcp::detail::Json wait_terminal(const job_data& job,
                                       std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    auto json = get_job(job, deadline);
    if (terminal(update(job, json))) return json;
    pause(*job.client, deadline);
  }
  throw error("Timed out waiting for Batch job to become terminal");
}

inline gcp::detail::Json cancel_job(const job_data& job) {
  const auto deadline = std::chrono::steady_clock::now() + job.client->config.cleanup_timeout;
  auto current = get_job(job, deadline);
  if (terminal(update(job, current))) return current;
  const std::string request_id = gcp::detail::random_uuid();
  gcp::detail::HttpResponse response;
  try {
    response =
        call(*job.client,
             gcp::detail::HttpRequest{
                 .method = "POST",
                 .url = job.client->core()->config.batch_endpoint + "/v1/" + job.name + ":cancel",
                 .headers = {"Content-Type: application/json"},
                 .body = "{\"requestId\":" + gcp::detail::json_quote(request_id) + "}",
             },
             deadline);
  } catch (const error& failure) {
    current = get_job(job, deadline);
    const job_state state = update(job, current);
    if (terminal(state)) return current;
    if (state == job_state::cancelling || retryable(failure.http_status()))
      return wait_terminal(job, deadline);
    throw;
  }
  wait_operation(*job.client, gcp::detail::field(gcp::detail::parse_json(response.body), "name"),
                 deadline);
  return wait_terminal(job, deadline);
}

inline result make_result(const job_data& job, const gcp::detail::Json& json) {
  const job_state state = update(job, json);
  result out{.state = state};
  out.warnings = job.chosen.warnings;
  if (state == job_state::succeeded) {
    out.exit_code = 0;
  } else if (state == job_state::failed) {
    out.exit_code = task_exit_code(job);
    out.message = status_error(json);
  } else if (state == job_state::cancelled) {
    out.message = "Batch job was cancelled";
  }
  return out;
}

}  // namespace detail

/*
 * Storage, Compute, and job handles
 */

class storage {
 public:
  object put(std::string_view destination, std::string_view bytes, put_options options = {}) const {
    const auto uri = detail::parse_uri(destination, "put");
    return raw().bucket(uri.bucket).put(uri.key, bytes, std::move(options));
  }

  object put_file(std::string_view destination, const std::filesystem::path& source,
                  put_options options = {}) const {
    const auto uri = detail::parse_uri(destination, "put_file");
    return raw().bucket(uri.bucket).put_file(uri.key, source, std::move(options));
  }

  [[nodiscard]] std::string get(std::string_view source) const {
    const auto uri = detail::parse_uri(source, "get");
    return raw().bucket(uri.bucket).get(uri.key);
  }

  void get_file(std::string_view source, const std::filesystem::path& destination) const {
    const auto uri = detail::parse_uri(source, "get_file");
    raw().bucket(uri.bucket).get_file(uri.key, destination);
  }

  [[nodiscard]] object_list list(std::string_view source, list_options options = {}) const {
    const auto uri = detail::parse_uri(source);
    options.prefix = uri.key + options.prefix;
    return raw().bucket(uri.bucket).list(std::move(options));
  }

  [[nodiscard]] object stat(std::string_view source) const {
    const auto uri = detail::parse_uri(source, "stat");
    return raw().bucket(uri.bucket).stat(uri.key);
  }

  void remove(std::string_view source) const {
    const auto uri = detail::parse_uri(source, "remove");
    raw().bucket(uri.bucket).erase(uri.key);
  }

 private:
  friend class client;
  explicit storage(std::shared_ptr<detail::client_state> state) : state_(std::move(state)) {}
  [[nodiscard]] gcp::Cloud& raw() const {
    (void)detail::selected_provider(state_->config);
    return state_->raw;
  }
  std::shared_ptr<detail::client_state> state_;
};

class compute {
 public:
  [[nodiscard]] std::vector<instance> instances(std::size_t limit = 0) const {
    return raw().vms(limit);
  }
  [[nodiscard]] operation create(std::string name, std::string instance_template) const {
    return raw().create_from_template(std::move(name), std::move(instance_template));
  }
  [[nodiscard]] operation start(std::string name) const {
    return raw().vm(std::move(name)).start();
  }
  [[nodiscard]] operation stop(std::string name) const { return raw().vm(std::move(name)).stop(); }
  [[nodiscard]] operation destroy(std::string name) const {
    return raw().vm(std::move(name)).erase();
  }

 private:
  friend class client;
  explicit compute(std::shared_ptr<detail::client_state> state) : state_(std::move(state)) {}
  [[nodiscard]] gcp::Cloud& raw() const {
    (void)detail::selected_provider(state_->config);
    return state_->raw;
  }
  std::shared_ptr<detail::client_state> state_;
};

class job {
 public:
  // Copies share this controller state. status(), logs(), wait(), and cancel()
  // must therefore be serialized by the caller; final log draining remains
  // best effort because Cloud Logging is eventually visible.
  [[nodiscard]] const std::string& id() const noexcept { return data_->id; }

  [[nodiscard]] job_state status() const {
    if (data_->cached) return data_->cached->state;
    return detail::update(*data_, detail::get_job(*data_));
  }

  [[nodiscard]] std::vector<log_entry> logs() const {
    if (!data_->cached) (void)detail::merge_logs(*data_);
    return data_->log_cache;
  }

  [[nodiscard]] result wait(log_sink sink = {}) const {
    if (data_->cached) return *data_->cached;
    std::string log_warning;
    const auto cleanup = [&](result& out, std::string_view reason) {
      data_->cached = out;
      if (!data_->spec.auto_delete) return;
      try {
        detail::delete_job(*data_, reason);
      } catch (const std::exception& failure) {
        out.warnings.push_back(std::string("automatic cleanup failed: ") + failure.what());
        data_->cached = out;
      }
    };
    const auto emit = [&](std::chrono::steady_clock::time_point deadline) {
      std::vector<log_entry> added;
      try {
        added = detail::merge_logs(*data_, deadline);
      } catch (const std::exception& failure) {
        log_warning = std::string("log polling failed: ") + failure.what();
        return std::size_t{0};
      }
      for (const auto& line : added)
        if (sink) {
          try {
            sink(line);
          } catch (...) {
            const auto original = std::current_exception();
            if (data_->spec.auto_delete) {
              try {
                const auto json = detail::cancel_job(*data_);
                result out = detail::make_result(*data_, json);
                data_->cached = out;
                detail::delete_job(*data_, "cloud.hpp log callback failure");
              } catch (...) {
              }
            }
            std::rethrow_exception(original);
          }
        }
      return added.size();
    };
    const auto drain = [&] { detail::drain_logs(*data_, emit); };
    const auto deadline = data_->submitted + data_->spec.timeout;
    try {
      while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
          const auto json = detail::cancel_job(*data_);
          drain();
          result out = detail::make_result(*data_, json);
          if (out.state == job_state::cancelled)
            out.message = "Cloud job exceeded its total timeout";
          if (!log_warning.empty()) out.warnings.push_back(log_warning);
          cleanup(out, "cloud.hpp timeout");
          return out;
        }
        (void)emit(deadline);
        if (std::chrono::steady_clock::now() >= deadline) continue;
        gcp::detail::Json json;
        try {
          json = detail::get_job(*data_, deadline);
        } catch (const error&) {
          if (std::chrono::steady_clock::now() >= deadline) continue;
          throw;
        }
        const job_state state = detail::update(*data_, json);
        if (detail::terminal(state)) {
          drain();
          result out = detail::make_result(*data_, json);
          if (!log_warning.empty()) out.warnings.push_back(log_warning);
          cleanup(out, "cloud.hpp automatic cleanup");
          return out;
        }
        detail::pause(*data_->client, deadline);
      }
    } catch (...) {
      const auto original = std::current_exception();
      if (data_->spec.auto_delete && !data_->cached) {
        try {
          const auto json = detail::cancel_job(*data_);
          result out = detail::make_result(*data_, json);
          cleanup(out, "cloud.hpp control failure");
        } catch (...) {
        }
      }
      std::rethrow_exception(original);
    }
  }

  void cancel() const {
    if (data_->cached) return;
    const auto json = detail::cancel_job(*data_);
    std::string warning;
    try {
      detail::drain_logs(
          *data_, [&](auto deadline) { return !detail::merge_logs(*data_, deadline).empty(); });
    } catch (const std::exception& failure) {
      warning = std::string("final log polling failed: ") + failure.what();
    }
    data_->cached = detail::make_result(*data_, json);
    if (!warning.empty()) data_->cached->warnings.push_back(std::move(warning));
    if (data_->spec.auto_delete) {
      try {
        detail::delete_job(*data_, "cloud.hpp cancellation");
      } catch (const std::exception& failure) {
        data_->cached->warnings.push_back(std::string("automatic cleanup failed: ") +
                                          failure.what());
      }
    }
  }

 private:
  friend class client;
  explicit job(std::shared_ptr<detail::job_data> data) : data_(std::move(data)) {}
  std::shared_ptr<detail::job_data> data_;
};

class client {
 public:
  explicit client(cloud::config value = {})
      : state_(std::make_shared<detail::client_state>(std::move(value))),
        storage_(state_),
        compute_(state_) {}

  [[nodiscard]] cloud::plan plan(const job_spec& spec) const {
    return detail::make_plan(state_->config, spec);
  }

  [[nodiscard]] cloud::job run(const job_spec& spec) const {
    const cloud::plan chosen = plan(spec);
    const std::string id = detail::job_id(spec.name);
    const std::string project = state_->core()->project();
    detail::validate_project(project);
    const std::string parent = "projects/" + project + "/locations/" + chosen.region + "/jobs";
    const std::string name = parent + '/' + id;
    const std::string request_id = gcp::detail::random_uuid();
    const std::string create_url = state_->core()->config.batch_endpoint + "/v1/projects/" +
                                   gcp::detail::encode(project) + "/locations/" +
                                   gcp::detail::encode(chosen.region) + "/jobs" +
                                   "?jobId=" + gcp::detail::encode(id) + "&requestId=" + request_id;
    const std::string body = detail::batch_body(spec, chosen);
    gcp::detail::HttpResponse created_response;
    try {
      created_response = detail::call(*state_, gcp::detail::HttpRequest{
                                                   .method = "POST",
                                                   .url = create_url,
                                                   .headers = {"Content-Type: application/json"},
                                                   .body = body,
                                               });
    } catch (const error& failure) {
      if (!detail::retryable(failure.http_status())) throw;
      const auto submission = std::current_exception();
      const auto recovery =
          std::min(state_->config.cleanup_timeout, std::chrono::milliseconds(30'000));
      const auto deadline = std::chrono::steady_clock::now() + recovery;
      while (true) {
        try {
          created_response =
              detail::call(*state_,
                           gcp::detail::HttpRequest{
                               .url = state_->core()->config.batch_endpoint + "/v1/" + name,
                           },
                           deadline);
          break;
        } catch (const error& lookup) {
          if (lookup.http_status() != 404 || std::chrono::steady_clock::now() >= deadline)
            std::rethrow_exception(submission);
          detail::pause(*state_, deadline);
        }
      }
    }
    const auto created = gcp::detail::parse_json(created_response.body);
    auto data = std::make_shared<detail::job_data>();
    data->client = state_;
    data->spec = spec;
    data->chosen = chosen;
    data->id = id;
    data->name = gcp::detail::field(created, "name");
    if (data->name.empty()) data->name = name;
    data->uid = gcp::detail::field(created, "uid");
    return cloud::job(std::move(data));
  }

  [[nodiscard]] bool supports(std::string_view value, feature requested) const {
    if (value != "gcp") return false;
    if (requested == feature::accelerators) return false;
    if (requested == feature::cost_estimates)
      return static_cast<bool>(state_->config.estimate_hourly_cost);
    return true;
  }

  [[nodiscard]] bool supports(feature requested) const {
    try {
      return supports(detail::selected_provider(state_->config), requested);
    } catch (const error&) {
      return false;
    }
  }

  [[nodiscard]] cloud::storage& storage() noexcept { return storage_; }
  [[nodiscard]] const cloud::storage& storage() const noexcept { return storage_; }
  [[nodiscard]] cloud::compute& compute() noexcept { return compute_; }
  [[nodiscard]] const cloud::compute& compute() const noexcept { return compute_; }
  [[nodiscard]] gcp::Cloud& gcp() noexcept { return state_->raw; }
  [[nodiscard]] const gcp::Cloud& gcp() const noexcept { return state_->raw; }

 private:
  std::shared_ptr<detail::client_state> state_;
  cloud::storage storage_;
  cloud::compute compute_;
};

}  // namespace cloud

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif  // CLOUD_HPP_INCLUDED
