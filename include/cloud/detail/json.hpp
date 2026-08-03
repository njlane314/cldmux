#pragma once

#include "cloud/detail/config.hpp"

namespace cloud::gcp {

// Internal encoding and JSON ---------------------------------------------------

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

// std::basic_string[_view]::starts_with()/ends_with() arrived in C++20. Keep
// their small, allocation-free equivalents private so every supported language
// mode follows exactly the same path.
inline bool starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

inline bool ends_with(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::string trim(std::string value) {
    const auto not_space = [](char c) { return !is_ascii_space(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

inline std::string env(std::string_view name) {
    const std::string key(name);
    if (const char* value = std::getenv(key.c_str()))
        return value;
    return {};
}

inline std::string header(std::string_view name, std::string_view value) {
    if (value.find('\r') != std::string_view::npos || value.find('\n') != std::string_view::npos)
        throw Error("Invalid newline in HTTP header value");
    return std::string(name) + ": " + std::string(value);
}

inline std::string base_url(std::string value) {
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

inline std::string encode(std::string_view input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() + input.size() / 4);
    for (const char raw : input) {
        const auto c = static_cast<unsigned char>(raw);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
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
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
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

// A deliberately small read-only JSON value. Numbers retain their original
// spelling so identifiers and provider counters are never rounded through a
// floating-point representation.
class Json {
public:
    struct Number {
        std::string text;
    };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    using Value = std::variant<std::nullptr_t, bool, std::string, Number, Array, Object>;

    Json() : value_(nullptr) {}
    explicit Json(Value value) : value_(std::move(value)) {}

    [[nodiscard]] bool is_object() const { return std::holds_alternative<Object>(value_); }

    [[nodiscard]] const Object& object() const {
        static const Object empty;
        if (const auto* p = std::get_if<Object>(&value_))
            return *p;
        return empty;
    }

    [[nodiscard]] const Array& array() const {
        static const Array empty;
        if (const auto* p = std::get_if<Array>(&value_))
            return *p;
        return empty;
    }

    [[nodiscard]] const Json* get(std::string_view key) const {
        const auto* obj = std::get_if<Object>(&value_);
        if (!obj)
            return nullptr;
        const auto it = obj->find(key);
        return it == obj->end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::string text(std::string fallback = {}) const {
        if (const auto* p = std::get_if<std::string>(&value_))
            return *p;
        if (const auto* p = std::get_if<Number>(&value_))
            return p->text;
        return fallback;
    }

    [[nodiscard]] bool boolean(bool fallback = false) const {
        if (const auto* p = std::get_if<bool>(&value_))
            return *p;
        return fallback;
    }

private:
    Value value_;
};

// Provider control responses are untrusted. These private defaults bound both
// the input retained for parsing and the tree built from it; callers never need
// to configure or interact with the wire codec.
struct JsonLimits {
    std::size_t max_bytes = 16 * 1024 * 1024;
    std::size_t max_depth = 64;
    std::size_t max_nodes = 262'144;
};

// Strict recursive-descent parser for provider responses. It accepts exactly
// JSON whitespace/number/string grammar, converts Unicode escapes to UTF-8, and
// stops before hostile nesting or collection sizes can exhaust local memory.
class JsonParser {
public:
    explicit JsonParser(std::string_view input, JsonLimits limits = {})
        : input_(input), limits_(limits) {}

    Json parse() {
        if (input_.size() > limits_.max_bytes)
            fail("response exceeds byte limit");
        whitespace();
        Json value = parse_value(0);
        whitespace();
        if (position_ != input_.size())
            fail("trailing characters");
        return value;
    }

private:
    [[noreturn]] void fail(std::string_view why) const {
        throw Error("Invalid JSON at byte " + std::to_string(position_) + ": " + std::string(why));
    }

    void whitespace() {
        while (position_ < input_.size() && is_json_space(input_[position_]))
            ++position_;
    }

    char take() {
        if (position_ >= input_.size())
            fail("unexpected end");
        return input_[position_++];
    }

    bool consume(std::string_view token) {
        if (input_.substr(position_, token.size()) != token)
            return false;
        position_ += token.size();
        return true;
    }

    Json parse_value(std::size_t depth) {
        if (depth > limits_.max_depth)
            fail("nesting limit exceeded");
        if (nodes_ == limits_.max_nodes)
            fail("node limit exceeded");
        ++nodes_;
        whitespace();
        if (position_ >= input_.size())
            fail("expected value");
        switch (input_[position_]) {
        case 'n':
            if (consume("null"))
                return Json(nullptr);
            break;
        case 't':
            if (consume("true"))
                return Json(true);
            break;
        case 'f':
            if (consume("false"))
                return Json(false);
            break;
        case '"':
            return Json(parse_string());
        case '[':
            return parse_array(depth);
        case '{':
            return parse_object(depth);
        default:
            if (input_[position_] == '-' || is_ascii_digit(input_[position_]))
                return parse_number();
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
        if (take() != '"')
            fail("expected string");
        std::string out;
        while (true) {
            const unsigned char c = static_cast<unsigned char>(take());
            if (c == '"')
                return out;
            if (c < 0x20)
                fail("control character in string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            switch (take()) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::uint32_t cp = hex4();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (take() != '\\' || take() != 'u')
                        fail("invalid surrogate pair");
                    const std::uint32_t low = hex4();
                    if (low < 0xdc00 || low > 0xdfff)
                        fail("invalid surrogate pair");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    fail("unpaired low surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default:
                fail("invalid string escape");
            }
        }
    }

    Json parse_number() {
        const std::size_t begin = position_;
        if (input_[position_] == '-')
            ++position_;
        if (position_ >= input_.size())
            fail("invalid number");
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
        if (position_ >= input_.size() || !is_ascii_digit(input_[position_]))
            fail(error);
        while (position_ < input_.size() && is_ascii_digit(input_[position_]))
            ++position_;
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
            if (separator == close)
                return Json(std::move(out));
            if (separator != ',')
                fail(separator_error);
            whitespace();
        }
    }

    Json parse_array(std::size_t depth) {
        return parse_collection(']', "expected ',' or ']'", Json::Array{},
                                [&](auto& out) { out.push_back(parse_value(depth + 1)); });
    }

    Json parse_object(std::size_t depth) {
        return parse_collection('}', "expected ',' or '}'", Json::Object{}, [&](auto& out) {
            if (position_ >= input_.size() || input_[position_] != '"')
                fail("expected object key");
            std::string key = parse_string();
            whitespace();
            if (take() != ':')
                fail("expected ':'");
            whitespace();
            out.emplace(std::move(key), parse_value(depth + 1));
        });
    }

    std::string_view input_;
    JsonLimits limits_;
    std::size_t position_ = 0;
    std::size_t nodes_ = 0;
};

inline Json parse_json(std::string_view text, JsonLimits limits = {}) {
    return JsonParser(text, limits).parse();
}

inline std::string field(const Json& value, std::string_view key) {
    const Json* child = value.get(key);
    return child ? child->text() : std::string{};
}

template <typename Function>
inline void for_each_json(const Json& value, std::string_view key, Function function) {
    if (const auto* items = value.get(key))
        for (const auto& item : items->array())
            function(item);
}

template <typename Item, typename Fetch, typename Parse, typename Visit>
inline std::vector<Item> paginate(std::size_t limit, Fetch fetch, Parse parse, Visit visit) {
    // Fetch owns the request, Parse converts each "items" entry, and Visit can
    // collect side channels such as common prefixes. A nonzero limit stops
    // before requesting another page once enough concrete items are present.
    std::vector<Item> result;
    std::string page;
    do {
        const Json json = fetch(page);
        for_each_json(json, "items", [&](const Json& item) {
            if (!limit || result.size() < limit)
                result.push_back(parse(item));
        });
        visit(json);
        page = limit && result.size() >= limit ? std::string{} : field(json, "nextPageToken");
    } while (!page.empty());
    return result;
}

inline std::uint64_t unsigned_field(const Json& value, std::string_view key) {
    const std::string text = field(value, key);
    if (text.empty())
        return 0;
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw Error("Invalid unsigned integer in JSON field '" + std::string(key) + "'");
    return result;
}

inline std::string last_path_segment(std::string value) {
    const auto slash = value.find_last_of('/');
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

// GCS exposes CRC32C as base64-encoded, big-endian bytes. Keeping the small
// Castagnoli implementation here avoids another runtime dependency.
inline std::uint32_t crc32c_update(std::uint32_t crc, const unsigned char* data, std::size_t size) {
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
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

// File uploads are pre-sized and optionally checksummed before libcurl sees
// them. The shared state keeps the stream alive across retryable requests.
struct UploadSource {
    std::filesystem::path path;
    std::shared_ptr<std::ifstream> stream;
    curl_off_t size = 0;
    std::string crc32c;
};

inline std::shared_ptr<UploadSource> prepare_upload(const std::filesystem::path& path,
                                                    bool checksum) {
    auto stream = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*stream)
        throw Error("Cannot open upload file: " + path.string());
    stream->seekg(0, std::ios::end);
    const std::streampos end = stream->tellg();
    if (end == std::streampos(-1))
        throw Error("Cannot determine upload file size: " + path.string());
    const std::streamoff length = end;
    if (length < 0 || static_cast<std::uintmax_t>(length) >
                          static_cast<std::uintmax_t>(std::numeric_limits<curl_off_t>::max()))
        throw Error("Upload file is too large for libcurl: " + path.string());
    stream->seekg(0, std::ios::beg);
    if (!*stream)
        throw Error("Cannot seek upload file: " + path.string());

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
        if (stream->bad())
            throw Error("Failed while checksumming upload file: " + path.string());
        digest = crc32c_base64(crc);
        stream->clear();
        stream->seekg(0, std::ios::beg);
        if (!*stream)
            throw Error("Cannot rewind upload file: " + path.string());
    }
    return std::make_shared<UploadSource>(
        UploadSource{path, std::move(stream), static_cast<curl_off_t>(length), std::move(digest)});
}

// RFC 4122 version-4 spelling used for provider idempotency/request IDs. These
// IDs prevent accidental duplicate mutations; they are not authentication data.
inline std::string random_uuid() {
    thread_local std::mt19937_64 random = [] {
        std::random_device source;
        std::array<std::uint32_t, 8> seed{};
        for (auto& word : seed)
            word = source();
        std::seed_seq sequence(seed.begin(), seed.end());
        return std::mt19937_64(sequence);
    }();
    std::uniform_int_distribution<unsigned> byte(0, 255);
    std::array<unsigned char, 16> data{};
    for (auto& value : data)
        value = static_cast<unsigned char>(byte(random));
    data[6] = static_cast<unsigned char>((data[6] & 0x0f) | 0x40);
    data[8] = static_cast<unsigned char>((data[8] & 0x3f) | 0x80);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out << '-';
        out << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return out.str();
}

} // namespace detail
} // namespace cloud::gcp
