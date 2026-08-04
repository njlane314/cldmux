#include "dispatch.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cldmux>

#include <algorithm>
#include <array>
#if !defined(DISPATCH_TESTING) && !defined(DISPATCH_NO_MAIN)
#include <charconv>
#endif
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#if !defined(DISPATCH_TESTING) && !defined(DISPATCH_NO_MAIN)
#include <iostream>
#include <locale>
#endif
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace dispatch {
namespace {

constexpr std::string_view receipt_version = "1";

struct roots {
    std::string input;
    std::string output;
};

std::string environment(std::string_view name) {
    const std::string key(name);
    if (const char* value = std::getenv(key.c_str()))
        return value;
    return {};
}

std::string storage_root(std::string value, std::string_view variable) {
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    constexpr std::string_view scheme = "cloud://";
    if (value.size() <= scheme.size() || value.substr(0, scheme.size()) != scheme ||
        value.find('/', scheme.size()) != std::string::npos)
        throw std::invalid_argument(std::string(variable) +
                                    " must be a cloud:// bucket or container root");
    return value;
}

roots configured_roots() {
    std::string input = environment("DISPATCH_INPUT_ROOT");
    std::string output = environment("DISPATCH_OUTPUT_ROOT");
    if (input.empty() || output.empty())
        throw std::invalid_argument(
            "distinct DISPATCH_INPUT_ROOT and DISPATCH_OUTPUT_ROOT values are required");
    roots result{storage_root(std::move(input), "DISPATCH_INPUT_ROOT"),
                 storage_root(std::move(output), "DISPATCH_OUTPUT_ROOT")};
    if (result.input == result.output)
        throw std::invalid_argument(
            "DISPATCH_INPUT_ROOT and DISPATCH_OUTPUT_ROOT must name different buckets or containers");
    return result;
}

bool safe_id(std::string_view value) {
    if (value.empty() || value.size() > 64 || value == "." || value == "..")
        return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
}

bool known_policy(std::string_view value) {
    return value == "cheapest" || value == "gcp" || value == "aws" || value == "azure";
}

std::filesystem::path normal_path(const std::filesystem::path& value) {
    std::error_code failure;
    const auto result = std::filesystem::weakly_canonical(
        std::filesystem::absolute(value).lexically_normal(), failure);
    if (failure)
        throw std::invalid_argument("cannot resolve local path: " + failure.message());
#if defined(_WIN32)
    const auto native = result.native();
    if (native.rfind(LR"(\\?\)", 0) == 0 || native.rfind(LR"(\\.\)", 0) == 0)
        throw std::invalid_argument("Windows device and extended-length paths are unsupported");
    for (const auto& part : result.relative_path()) {
        const auto component = part.native();
        if (!component.empty() &&
            (component.back() == L'.' || component.back() == L' ' ||
             component.find(L':') != std::wstring::npos))
            throw std::invalid_argument(
                "Windows path components may not end in dot or space or contain a colon");
    }
#endif
    return result;
}

bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32)
    const int comparison =
        CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE);
    if (!comparison) {
        const std::error_code failure(static_cast<int>(GetLastError()), std::system_category());
        throw std::runtime_error("cannot compare local paths: " + failure.message());
    }
    return comparison == CSTR_EQUAL;
#else
    return left == right;
#endif
}

std::filesystem::path parent_or_current(const std::filesystem::path& value) {
    return value.parent_path().empty() ? std::filesystem::path(".") : value.parent_path();
}

void require_parent_directory(const std::filesystem::path& value, std::string_view description) {
    const auto parent = parent_or_current(value);
    std::error_code failure;
    if (!std::filesystem::is_directory(parent, failure) || failure)
        throw std::invalid_argument(std::string(description) +
                                    " parent must be an existing directory");
}

std::filesystem::path default_receipt_path(const std::filesystem::path& output) {
    std::filesystem::path result = output;
    result += ".receipt";
    return result;
}

void validate_local_request(request& value) {
    if (!safe_id(value.id))
        throw std::invalid_argument(
            "request id must contain 1-64 letters, digits, dots, underscores, or hyphens");
    if (!known_policy(value.policy))
        throw std::invalid_argument("policy must be cheapest, gcp, aws, or azure");
    if (!value.catalogue_pricing && value.policy == "cheapest")
        throw std::invalid_argument("cheapest policy requires catalogue pricing");
    if (!value.gpu.empty() && value.policy == "cheapest")
        throw std::invalid_argument("mounted GPU jobs require an explicit gcp or azure policy");
    if (value.input_bundle.empty() || value.output_bundle.empty())
        throw std::invalid_argument("input and output bundle paths are required");
    if (value.receipt_file.empty())
        value.receipt_file = default_receipt_path(value.output_bundle);

    std::error_code failure;
    if (!std::filesystem::is_regular_file(value.input_bundle, failure) || failure)
        throw std::invalid_argument("input bundle must be an existing regular file");
    require_parent_directory(value.output_bundle, "output bundle");
    require_parent_directory(value.receipt_file, "receipt");

    const auto input = normal_path(value.input_bundle);
    const auto output = normal_path(value.output_bundle);
    const auto receipt_path = normal_path(value.receipt_file);
    std::filesystem::path pending_path = receipt_path;
    pending_path += ".pending";
    if (same_path(input, output) || same_path(input, receipt_path) ||
        same_path(output, receipt_path) || same_path(input, pending_path) ||
        same_path(output, pending_path))
        throw std::invalid_argument(
            "input, output, receipt, and pending receipt paths must be distinct");
    if (std::filesystem::exists(output))
        throw std::invalid_argument("output bundle already exists");
    if (std::filesystem::exists(receipt_path))
        throw std::invalid_argument("receipt already exists");
    if (std::filesystem::exists(pending_path))
        throw std::invalid_argument("pending dispatch receipt already exists");

    // Pin paths at prepare time; changing the process working directory must
    // not redirect a previously approved transaction.
    value.input_bundle = input;
    value.output_bundle = output;
    value.receipt_file = receipt_path;
}

std::string image_digest(std::string_view image) {
    constexpr std::string_view marker = "@sha256:";
    const std::size_t marker_position = image.rfind(marker);
    if (marker_position == std::string_view::npos ||
        image.size() - marker_position != marker.size() + 64)
        return {};
    std::string result = "sha256:";
    result.reserve(71);
    for (const char digit : image.substr(marker_position + marker.size())) {
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
              (digit >= 'A' && digit <= 'F')))
            return {};
        result.push_back(digit >= 'A' && digit <= 'F' ? static_cast<char>(digit - 'A' + 'a')
                                                      : digit);
    }
    return result;
}

std::string join_uri(std::string_view root, std::string_view key) {
    return std::string(root) + '/' + std::string(key);
}

quote public_quote(const cldmux::plan& value) {
    return {value.provider, value.region, value.machine_type, value.estimated_hourly_cost,
            value.warnings};
}

std::string job_state_name(cldmux::job_state value) {
    switch (value) {
    case cldmux::job_state::queued:
        return "queued";
    case cldmux::job_state::scheduled:
        return "scheduled";
    case cldmux::job_state::running:
        return "running";
    case cldmux::job_state::succeeded:
        return "succeeded";
    case cldmux::job_state::failed:
        return "failed";
    case cldmux::job_state::cancelling:
        return "cancelling";
    case cldmux::job_state::cancelled:
        return "cancelled";
    case cldmux::job_state::deleting:
        return "deleting";
    case cldmux::job_state::unknown:
        return "unknown";
    }
    return "unknown";
}

// Small app-local SHA-256 implementation keeps receipts portable without a
// shell command or a second cryptographic dependency.
class sha256 {
public:
    void update(const unsigned char* data, std::size_t size) {
        if (size > (std::numeric_limits<std::uint64_t>::max)() - total_bytes_)
            throw std::runtime_error("artefact is too large to hash");
        total_bytes_ += static_cast<std::uint64_t>(size);
        while (size) {
            const std::size_t count = (std::min)(size, block_.size() - used_);
            std::copy_n(data, count, block_.begin() + static_cast<std::ptrdiff_t>(used_));
            used_ += count;
            data += count;
            size -= count;
            if (used_ == block_.size()) {
                transform(block_.data());
                used_ = 0;
            }
        }
    }

    [[nodiscard]] std::string finish() const {
        sha256 copy = *this;
        if (copy.total_bytes_ > (std::numeric_limits<std::uint64_t>::max)() / 8)
            throw std::runtime_error("artefact is too large to hash");
        const std::uint64_t bit_count = copy.total_bytes_ * 8;
        copy.block_[copy.used_++] = 0x80;
        if (copy.used_ > 56) {
            std::fill(copy.block_.begin() + static_cast<std::ptrdiff_t>(copy.used_),
                      copy.block_.end(), 0);
            copy.transform(copy.block_.data());
            copy.used_ = 0;
        }
        std::fill(copy.block_.begin() + static_cast<std::ptrdiff_t>(copy.used_),
                  copy.block_.begin() + 56, 0);
        for (unsigned index = 0; index < 8; ++index)
            copy.block_[63 - index] =
                static_cast<unsigned char>(bit_count >> static_cast<unsigned>(index * 8));
        copy.transform(copy.block_.data());

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const std::uint32_t word : copy.state_)
            output << std::setw(8) << word;
        return output.str();
    }

private:
    static constexpr std::array<std::uint32_t, 64> constants_ = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };

    static std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
        return (value >> count) | (value << (32U - count));
    }

    void transform(const unsigned char* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = index * 4;
            words[index] = static_cast<std::uint32_t>(block[offset]) << 24U |
                           static_cast<std::uint32_t>(block[offset + 1]) << 16U |
                           static_cast<std::uint32_t>(block[offset + 2]) << 8U |
                           static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t first = rotate_right(words[index - 15], 7) ^
                                        rotate_right(words[index - 15], 18) ^
                                        (words[index - 15] >> 3U);
            const std::uint32_t second = rotate_right(words[index - 2], 17) ^
                                         rotate_right(words[index - 2], 19) ^
                                         (words[index - 2] >> 10U);
            words[index] = words[index - 16] + first + words[index - 7] + second;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 =
                rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sum1 + choice + constants_[index] + words[index];
            const std::uint32_t sum0 =
                rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                           0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t total_bytes_ = 0;
};

struct fingerprint {
    std::uint64_t size = 0;
    std::string sha256;
};

fingerprint fingerprint_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open artefact for hashing: " + path.string());
    sha256 hash;
    std::uint64_t size = 0;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read > 0) {
            const auto count = static_cast<std::uint64_t>(read);
            if (count > (std::numeric_limits<std::uint64_t>::max)() - size)
                throw std::runtime_error("artefact is too large to measure");
            size += count;
            hash.update(reinterpret_cast<const unsigned char*>(buffer.data()),
                        static_cast<std::size_t>(read));
        }
    }
    if (!input.eof())
        throw std::runtime_error("cannot finish hashing artefact: " + path.string());
    return {size, hash.finish()};
}

class temporary_file {
public:
    temporary_file(const std::filesystem::path& destination, std::string_view purpose) {
        std::random_device random;
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            std::ostringstream suffix;
            suffix << std::hex << random() << random() << attempt;
            const auto candidate = parent_or_current(destination) /
                                   ('.' + destination.filename().string() + ".dispatch-" +
                                    std::string(purpose) + '-' + suffix.str() + ".tmpdir");
            std::error_code failure;
            if (!std::filesystem::create_directory(candidate, failure)) {
                if (!failure || failure == std::errc::file_exists)
                    continue;
                throw std::runtime_error("cannot create temporary directory: " +
                                         failure.message());
            }
            directory_ = candidate;
            std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, failure);
            if (failure) {
                std::error_code ignored;
                std::filesystem::remove(directory_, ignored);
                throw std::runtime_error("cannot set temporary directory permissions: " +
                                         failure.message());
            }
            path_ = directory_ / "content";
            return;
        }
        throw std::runtime_error("cannot choose a temporary artefact directory");
    }
    temporary_file(temporary_file&& other) noexcept
        : directory_(std::move(other.directory_)), path_(std::move(other.path_)) {
        other.directory_.clear();
        other.path_.clear();
    }
    temporary_file(const temporary_file&) = delete;
    temporary_file& operator=(const temporary_file&) = delete;
    temporary_file& operator=(temporary_file&&) = delete;
    ~temporary_file() {
        if (directory_.empty())
            return;
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    void retain() noexcept {
        directory_.clear();
        path_.clear();
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

std::string available(std::string_view value) {
    return value.empty() ? "unavailable" : std::string(value);
}

std::string execution_token() {
    std::random_device random;
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned index = 0; index < 4; ++index)
        output << std::setw(8) << random();
    return output.str();
}

cldmux::command_output receipt_records(const receipt& value, std::string_view status) {
    cldmux::command_output output;
    output.add("receipt_version", receipt_version);
    output.add("receipt_status", status);
    output.add("request_id", value.request_id);
    output.add("execution_id", available(value.execution_id));
    output.add("run_id", available(value.run_id));
    output.add("image_reference", value.image_reference);
    output.add("image_digest", available(value.image_digest));
    output.add("input_uri", value.remote_input);
    if (value.input_sha256.empty()) {
        output.add("input_size_bytes", "unavailable");
        output.add("input_sha256", "unavailable");
    } else {
        output.add_unsigned("input_size_bytes", value.input_size_bytes);
        output.add("input_sha256", value.input_sha256);
    }
    output.add("input_generation", available(value.input_generation));
    output.add("input_crc32c", available(value.input_crc32c));
    output.add("provider", value.selected.provider);
    output.add("region", value.selected.region);
    output.add("machine", value.selected.machine);
    if (value.selected.hourly_usd)
        output.add_number("quote_hourly_usd", *value.selected.hourly_usd);
    else
        output.add("quote_hourly_usd", "unavailable");
    output.add("quote_kind", "advisory-planning-snapshot");
    output.add_integer("elapsed_milliseconds", value.elapsed.count());
    output.add("job_state", value.job_state);
    output.add("transaction_phase", value.transaction_phase);
    output.add("transaction_error", available(value.transaction_error));
    output.add_boolean("job_succeeded", value.job_succeeded);
    output.add_boolean("output_retrieved", value.output_retrieved);
    output.add_boolean("success", value.success);
    if (value.exit_code)
        output.add_integer("exit_code", *value.exit_code);
    else
        output.add("exit_code", "unavailable");
    output.add("message", value.message);
    output.add("output_uri", value.remote_output);
    if (value.output_sha256.empty()) {
        output.add("output_size_bytes", "unavailable");
        output.add("output_sha256", "unavailable");
    } else {
        output.add_unsigned("output_size_bytes", value.output_size_bytes);
        output.add("output_sha256", value.output_sha256);
    }
    output.add("output_generation", available(value.output_generation));
    output.add("output_crc32c", available(value.output_crc32c));
    output.add("receipt_file", value.receipt_file.string());
    output.add("recovery_file", available(value.recovery_file.string()));
    output.add_boolean("receipt_persisted", value.receipt_persisted);
    for (const auto& warning : value.selected.warnings)
        output.add("warning", warning);
    for (const auto& warning : value.warnings)
        output.add("warning", warning);
    return output;
}

temporary_file write_receipt_temporary(const receipt& value, std::string_view status,
                                       const std::filesystem::path& destination) {
    temporary_file temporary(destination, "receipt");
    std::ofstream output(temporary.path(), std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open temporary receipt: " + temporary.path().string());
    receipt_records(value, status).write(output);
    output.flush();
    if (!output)
        throw std::runtime_error("cannot write temporary receipt: " + temporary.path().string());
    output.close();
    if (!output)
        throw std::runtime_error("cannot close temporary receipt: " + temporary.path().string());
    return temporary;
}

void publish_no_clobber(const std::filesystem::path& temporary,
                        const std::filesystem::path& destination, std::string_view description) {
    std::error_code failure;
    std::filesystem::create_hard_link(temporary, destination, failure);
    if (failure)
        throw std::runtime_error("cannot publish " + std::string(description) +
                                 " without overwriting: " + failure.message());
}

void replace_existing(const std::filesystem::path& temporary,
                      const std::filesystem::path& destination, std::string_view description) {
#if defined(_WIN32)
    temporary_file backup(destination, "backup");
    // ReplaceFileW can move the old file even when it reports failure. A named
    // backup lets us restore it or leave a recoverable copy outside RAII cleanup.
    if (ReplaceFileW(destination.c_str(), temporary.c_str(), backup.path().c_str(), 0, nullptr,
                     nullptr))
        return;
    const DWORD replacement_code = GetLastError();
    const std::error_code failure(static_cast<int>(replacement_code), std::system_category());
    if (replacement_code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
        if (MoveFileExW(backup.path().c_str(), destination.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace " + std::string(description) + ": " +
                                     failure.message() + "; previous file restored");
        const std::error_code restore_failure(static_cast<int>(GetLastError()),
                                              std::system_category());
        const auto retained = backup.path();
        backup.retain();
        throw std::runtime_error("cannot replace " + std::string(description) + ": " +
                                 failure.message() + "; cannot restore previous file: " +
                                 restore_failure.message() + "; previous file retained at " +
                                 retained.string());
    }
#else
    std::error_code failure;
    std::filesystem::rename(temporary, destination, failure);
    if (!failure)
        return;
#endif
    throw std::runtime_error("cannot replace " + std::string(description) + ": " +
                             failure.message());
}

bool pending_owner(const std::filesystem::path& path, std::string_view execution_id) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    const std::string wanted = "execution_id=" + std::string(execution_id);
    std::string line;
    while (std::getline(input, line))
        if (line == wanted)
            return true;
    return false;
}

// The no-clobber lease remains immutable. Mutable checkpoints use a path that
// includes the execution token, so a stale owner cannot overwrite a later run.
class execution_lease {
public:
    execution_lease(std::filesystem::path path, std::filesystem::path state_path,
                    const receipt& value)
        : path_(std::move(path)), state_path_(std::move(state_path)),
          execution_id_(value.execution_id) {
        auto temporary = write_receipt_temporary(value, "approved", path_);
        publish_no_clobber(temporary.path(), path_, "pending receipt lease");
        active_ = true;
    }

    execution_lease(const execution_lease&) = delete;
    execution_lease& operator=(const execution_lease&) = delete;

    [[nodiscard]] bool owns() const { return active_ && pending_owner(path_, execution_id_); }

    void update(const receipt& value, std::string_view status) {
        require_owner();
        auto temporary = write_receipt_temporary(value, status, state_path_);
        if (!state_created_) {
            publish_no_clobber(temporary.path(), state_path_, "pending receipt state");
            state_created_ = true;
            return;
        }
        replace_existing(temporary.path(), state_path_, "pending receipt");
    }

    void release() {
        require_owner();
        std::error_code failure;
        if (state_created_ && (!std::filesystem::remove(state_path_, failure) || failure))
            throw std::runtime_error("cannot remove owned recovery state: " + failure.message());
        require_owner();
        if (!std::filesystem::remove(path_, failure) || failure)
            throw std::runtime_error("cannot remove owned pending receipt: " + failure.message());
        active_ = false;
    }

private:
    void require_owner() const {
        if (!owns())
            throw std::runtime_error("pending receipt lease is no longer owned by this execution");
    }

    std::filesystem::path path_;
    std::filesystem::path state_path_;
    std::string execution_id_;
    bool active_ = false;
    bool state_created_ = false;
};

void persist_final(const receipt& value, std::string_view status) {
    auto temporary = write_receipt_temporary(value, status, value.receipt_file);
    publish_no_clobber(temporary.path(), value.receipt_file, "receipt");
}

bool object_exists(const cldmux::storage& storage, std::string_view uri) {
    try {
        (void)storage.stat(uri);
        return true;
    } catch (const cldmux::error& failure) {
        if (failure.http_status() == 404)
            return false;
        throw;
    }
}

bool same_object(const cldmux::object& first, const cldmux::object& second) {
    return !first.generation.empty() && first.generation == second.generation &&
           first.size == second.size && first.crc32c == second.crc32c;
}

} // namespace

struct prepared_run::implementation {
    implementation(request requested_value, cldmux::job_spec job_value,
                   cldmux::client bound_value, cldmux::plan selected_value,
                   quote quoted_value, std::string input_value, std::string output_value,
                   std::filesystem::path pending_value)
        : requested(std::move(requested_value)), job(std::move(job_value)),
          bound(std::move(bound_value)), selected(std::move(selected_value)),
          quoted(std::move(quoted_value)), input_uri(std::move(input_value)),
          output_uri(std::move(output_value)), pending_file(std::move(pending_value)) {}

    request requested;
    cldmux::job_spec job;
    cldmux::client bound;
    cldmux::plan selected;
    quote quoted;
    std::string input_uri;
    std::string output_uri;
    std::filesystem::path pending_file;
};

prepared_run::prepared_run(std::unique_ptr<implementation> value) noexcept
    : implementation_(std::move(value)) {}

prepared_run::prepared_run(prepared_run&&) noexcept = default;
prepared_run& prepared_run::operator=(prepared_run&&) noexcept = default;
prepared_run::~prepared_run() = default;

prepared_run::operator bool() const noexcept { return static_cast<bool>(implementation_); }

const quote& prepared_run::selected_quote() const {
    if (!implementation_)
        throw std::logic_error("prepared run has already been consumed");
    return implementation_->quoted;
}

const std::filesystem::path& prepared_run::receipt_path() const {
    if (!implementation_)
        throw std::logic_error("prepared run has already been consumed");
    return implementation_->requested.receipt_file;
}

prepared_run core::prepare(request value) const {
    validate_local_request(value);
    const roots artefact_roots = configured_roots();
    const std::string run_key = "runs/" + value.id;

    cldmux::job_spec job;
    job.name = "dispatch-" + value.id;
    job.image = value.image;
    job.command = value.command;
    job.mounts = {{artefact_roots.input, "/dispatch/input", true},
                  {artefact_roots.output, "/dispatch/output", false}};
    job.resources.cpus = value.cpus;
    job.resources.memory_gb = value.memory_gb;
    job.resources.gpu = value.gpu;
    job.resources.gpu_count = value.gpu_count;
    job.resources.spot = value.spot;
    job.resources.max_price_per_hour = value.max_hourly_usd;
    job.retries = value.retries;
    job.timeout = value.timeout;
    job.auto_delete = true;

    cldmux::router router =
        value.catalogue_pricing
            ? cldmux::router::from_environment(value.policy,
                                               cldmux::price_source::public_catalogue)
            : cldmux::router::from_environment(value.policy);
    cldmux::plan selected = router.plan(job);
    cldmux::client bound = router.route(selected.provider);
    quote quoted = public_quote(selected);
    quoted.warnings.push_back(
        "quote is an advisory planning snapshot; execute replans before mutation and run() "
        "replans before submission");
    quoted.warnings.push_back(
        "elapsed starts before run(); terminal results end at wait(), while recovery paths end "
        "after controller failure handling; it is not provider-billable runtime");
    quoted.warnings.push_back(
        "portable images must use equivalent entrypoint semantics on every eligible provider");
    if (image_digest(value.image).empty())
        quoted.warnings.push_back("container image is not pinned by a recognised @sha256 digest");

    // Invoking execute approves this planning snapshot. A later increase then
    // fails closed both in the read-only refresh below and inside run().
    if (selected.estimated_hourly_cost) {
        const double quoted_price = *selected.estimated_hourly_cost;
        job.resources.max_price_per_hour =
            value.max_hourly_usd ? (std::min)(*value.max_hourly_usd, quoted_price) : quoted_price;
    }

    const std::string input_uri = join_uri(artefact_roots.input, run_key + "/input.tar.zst");
    const std::string output_uri = join_uri(artefact_roots.output, run_key + "/output.tar.zst");
    std::filesystem::path pending = value.receipt_file;
    pending += ".pending";

    return prepared_run(std::make_unique<prepared_run::implementation>(
        std::move(value), std::move(job), std::move(bound), std::move(selected), std::move(quoted),
        input_uri, output_uri, std::move(pending)));
}

receipt core::execute(prepared_run prepared, progress on_progress) const {
    if (!prepared.implementation_)
        throw std::invalid_argument("prepared run has already been consumed");
    auto state = std::move(prepared.implementation_);

    receipt value;
    value.request_id = state->requested.id;
    value.execution_id = execution_token();
    value.selected = state->quoted;
    value.image_reference = state->requested.image;
    value.image_digest = image_digest(state->requested.image);
    value.remote_input = state->input_uri;
    value.remote_output = state->output_uri;
    value.receipt_file = state->requested.receipt_file;
    value.recovery_file = state->pending_file;
    value.recovery_file += "." + value.execution_id;
    value.transaction_phase = "validating";

    std::optional<std::chrono::steady_clock::time_point> started;
    std::optional<cldmux::job> submitted;
    bool terminal_result = false;
    std::string progress_failure;
    const auto report_progress = [&](std::string_view text) {
        if (!on_progress)
            return;
        try {
            on_progress(text);
        } catch (const std::exception& failure) {
            if (progress_failure.empty())
                progress_failure = std::string("progress callback failed: ") + failure.what();
        } catch (...) {
            if (progress_failure.empty())
                progress_failure = "progress callback failed with a non-standard exception";
        }
    };

    validate_local_request(state->requested);
    value.recovery_file = normal_path(value.recovery_file);
    if (same_path(value.recovery_file, state->requested.input_bundle) ||
        same_path(value.recovery_file, state->requested.output_bundle) ||
        same_path(value.recovery_file, state->requested.receipt_file))
        throw std::invalid_argument("recovery state path collides with a transaction path");
    // Refresh against the approved quote ceiling before any remote mutation.
    const cldmux::plan refreshed = state->bound.plan(state->job);
    if (refreshed.provider != state->selected.provider)
        throw std::runtime_error("provider-bound refresh changed provider");
    const cldmux::storage storage = state->bound.storage();
    if (object_exists(storage, state->output_uri))
        throw std::runtime_error("remote output artefact already exists");

    value.transaction_phase = "approved";
    execution_lease lease(state->pending_file, value.recovery_file, value);

    try {
        value.transaction_phase = "staging-input";
        report_progress("stage=staging-input");
        temporary_file snapshot(value.receipt_file, "input");
        std::error_code copy_failure;
        std::filesystem::copy_file(state->requested.input_bundle, snapshot.path(),
                                   std::filesystem::copy_options::none, copy_failure);
        if (copy_failure)
            throw std::runtime_error("cannot snapshot input bundle: " + copy_failure.message());
        const fingerprint input = fingerprint_file(snapshot.path());
        value.input_size_bytes = input.size;
        value.input_sha256 = input.sha256;

        value.transaction_phase = "uploading-input";
        report_progress("stage=uploading-input");
        cldmux::put_options create_only;
        create_only.if_generation_match = "0";
        const cldmux::object uploaded =
            storage.put_file(state->input_uri, snapshot.path(), create_only);
        if (uploaded.size != input.size || uploaded.generation.empty() || uploaded.crc32c.empty())
            throw std::runtime_error(
                "uploaded input artefact lacks matching size, generation, or CRC32C metadata");
        value.input_generation = uploaded.generation;
        value.input_crc32c = uploaded.crc32c;
        value.transaction_phase = "input-staged";
        lease.update(value, "input-staged");

        value.transaction_phase = "submitting";
        value.job_state = "submission-unknown";
        lease.update(value, "submitting");
        report_progress("stage=submitting");
        started = std::chrono::steady_clock::now();
        submitted.emplace(state->bound.run(state->job));
        value.run_id = submitted->id();
        value.job_state = "submitted";
        value.transaction_phase = "submitted";
        try {
            lease.update(value, "submitted");
            report_progress("run_id=" + value.run_id);
        } catch (const std::exception& failure) {
            report_progress("run_id=" + value.run_id);
            value.warnings.push_back(std::string("submitted checkpoint failed: ") + failure.what());
            try {
                submitted->cancel();
                value.warnings.push_back(
                    "cancellation was requested because the run id could not be checkpointed");
            } catch (const std::exception& cancellation_failure) {
                value.warnings.push_back(std::string("checkpoint-failure cancellation failed: ") +
                                         cancellation_failure.what());
            }
            throw std::runtime_error(
                "submitted run id could not be persisted safely; execution was not awaited");
        }

        value.transaction_phase = "waiting";
        report_progress("stage=waiting");
        const cldmux::result result =
            submitted->wait([&](const cldmux::log_entry& entry) { report_progress(entry.text); });
        value.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - *started);
        terminal_result = true;
        value.job_state = job_state_name(result.state);
        value.job_succeeded = result.success();
        value.exit_code = result.exit_code;
        value.message = result.message;
        value.warnings.insert(value.warnings.end(), result.warnings.begin(), result.warnings.end());
        value.transaction_phase = "terminal";
        lease.update(value, "terminal");

        try {
            const cldmux::object current_input = storage.stat(state->input_uri);
            if (!same_object(uploaded, current_input))
                throw std::runtime_error("remote input artefact changed during execution");
        } catch (const std::exception& failure) {
            if (value.job_succeeded)
                throw;
            value.warnings.push_back(std::string("input immutability check failed: ") +
                                     failure.what());
        }

        if (value.job_succeeded) {
            value.transaction_phase = "retrieving-output";
            report_progress("stage=retrieving-output");
            const cldmux::object before = storage.stat(state->output_uri);
            temporary_file downloaded(state->requested.output_bundle, "output");
            storage.get_file(state->output_uri, downloaded.path());
            const cldmux::object after = storage.stat(state->output_uri);
            if (!same_object(before, after))
                throw std::runtime_error("remote output artefact changed during retrieval");
            const fingerprint output = fingerprint_file(downloaded.path());
            if (output.size != before.size)
                throw std::runtime_error("downloaded output size does not match remote metadata");
            value.output_size_bytes = before.size;
            value.output_generation = before.generation;
            value.output_crc32c = before.crc32c;
            value.output_sha256 = output.sha256;
            publish_no_clobber(downloaded.path(), state->requested.output_bundle, "output bundle");
            value.output_retrieved = true;
            value.transaction_phase = "complete";
            report_progress("stage=complete");
        } else {
            value.transaction_phase = "job-failed";
        }
    } catch (const std::exception& failure) {
        if (started && !terminal_result)
            value.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - *started);
        value.transaction_error = failure.what();
        if (submitted && !terminal_result) {
            value.transaction_phase = "recovery-required";
            value.warnings.push_back("provider job may still exist; use run_id for recovery and "
                                     "avoid blind resubmission");
        } else if (!submitted && started) {
            value.transaction_phase = "recovery-required";
            value.job_state = "submission-unknown";
            value.warnings.push_back(
                "submission outcome may be unknown; inspect the provider before retrying");
        } else {
            value.transaction_phase = "transaction-failed";
        }
    }

    if (!progress_failure.empty())
        value.warnings.push_back(progress_failure);
    value.success = value.job_succeeded && value.output_retrieved;

    const std::string receipt_status = value.success ? "complete"
                                       : value.transaction_phase == "recovery-required"
                                           ? "recovery-required"
                                           : "failed";
    try {
        lease.update(value, receipt_status);
    } catch (const std::exception& failure) {
        value.warnings.push_back(std::string("terminal pending receipt update failed: ") +
                                 failure.what());
    }

    if (!lease.owns()) {
        value.receipt_persisted = false;
        value.warnings.push_back(
            "final receipt was not published because this execution lost its pending lease");
        return value;
    }

    value.receipt_persisted = true;
    try {
        persist_final(value, receipt_status);
        try {
            lease.release();
        } catch (const std::exception& failure) {
            value.warnings.push_back(std::string("completed receipt retained a pending copy: ") +
                                     failure.what());
        }
    } catch (const std::exception& failure) {
        value.receipt_persisted = false;
        value.warnings.push_back(std::string("final receipt was not persisted: ") + failure.what());
        try {
            lease.update(value, receipt_status);
        } catch (const std::exception& pending_failure) {
            value.warnings.push_back(std::string("recovery receipt update also failed: ") +
                                     pending_failure.what());
        }
    }
    return value;
}

#if defined(DISPATCH_TESTING)
namespace testing {

std::string sha256_file(const std::filesystem::path& path) { return fingerprint_file(path).sha256; }

void replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& destination) {
    replace_existing(temporary, destination, "test file");
}

void persist_receipt(receipt value, const std::filesystem::path& path) {
    value.receipt_file = path;
    value.receipt_persisted = true;
    persist_final(value, "complete");
}

} // namespace testing
#endif

} // namespace dispatch

#if !defined(DISPATCH_TESTING) && !defined(DISPATCH_NO_MAIN)
namespace {

struct options {
    dispatch::request request;
    std::chrono::milliseconds expected_runtime = std::chrono::minutes(5);
    bool allow_unpriced = false;
    bool submit = false;
    bool help = false;
};

std::string escaped(std::string_view value) {
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size());
    for (const char byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (character < 0x20U || character == 0x7fU) {
                result += "\\x";
                result += hexadecimal[character >> 4U];
                result += hexadecimal[character & 0x0fU];
            } else {
                result += static_cast<char>(character);
            }
        }
    }
    return result;
}

void record(std::ostream& output, std::string_view key, std::string_view value) {
    output << key << '=' << escaped(value) << '\n';
    output.flush();
}

void record(std::ostream& output, std::string_view key, const char* value) {
    record(output, key, std::string_view(value));
}

void record(std::ostream& output, std::string_view key, bool value) {
    record(output, key, std::string_view(value ? "true" : "false"));
}

template <class Integer>
void record_integer(std::ostream& output, std::string_view key, Integer value) {
    record(output, key, std::to_string(value));
}

std::string number(double value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(15) << value;
    return output.str();
}

bool option_value(std::string_view argument, std::string_view name, std::string_view& value) {
    if (argument.size() < name.size() || argument.substr(0, name.size()) != name)
        return false;
    value = argument.substr(name.size());
    return true;
}

void set_once(bool& seen, std::string_view name) {
    if (std::exchange(seen, true))
        throw std::invalid_argument(std::string(name) + " may be supplied only once");
}

std::uint64_t parse_unsigned(std::string_view text, std::string_view description, bool allow_zero) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        (!allow_zero && value == 0))
        throw std::invalid_argument(std::string(description) + " must be a valid integer");
    return value;
}

unsigned parse_unsigned_option(std::string_view text, std::string_view description,
                               bool allow_zero) {
    const std::uint64_t value = parse_unsigned(text, description, allow_zero);
    if (value > (std::numeric_limits<unsigned>::max)())
        throw std::invalid_argument(std::string(description) + " is too large");
    return static_cast<unsigned>(value);
}

double parse_number(std::string_view text, std::string_view description, bool allow_zero) {
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    double value = 0;
    char trailing = '\0';
    if (!(input >> value) || (input >> trailing) || !std::isfinite(value) || value < 0 ||
        (!allow_zero && value == 0))
        throw std::invalid_argument(std::string(description) +
                                    " must be a finite, non-negative number");
    return value;
}

std::chrono::milliseconds parse_duration(std::string_view text, std::string_view description) {
    if (text.size() < 2)
        throw std::invalid_argument(std::string(description) +
                                    " requires a positive s, m, or h suffix");

    std::uint64_t multiplier = 0;
    switch (text.back()) {
    case 's':
        multiplier = 1'000;
        break;
    case 'm':
        multiplier = 60'000;
        break;
    case 'h':
        multiplier = 3'600'000;
        break;
    default:
        throw std::invalid_argument(std::string(description) + " requires an s, m, or h suffix");
    }

    const std::uint64_t quantity =
        parse_unsigned(text.substr(0, text.size() - 1), description, false);
    using representation = std::chrono::milliseconds::rep;
    const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<representation>::max)());
    if (quantity > maximum / multiplier)
        throw std::invalid_argument(std::string(description) + " is too large");
    return std::chrono::milliseconds(static_cast<representation>(quantity * multiplier));
}

void print_help(std::ostream& output) {
    output << "Usage: dispatch --id=ID --image=IMAGE --input=FILE --output=FILE [OPTIONS] -- "
              "COMMAND [ARGUMENT ...]\n"
           << "\n"
           << "Prepare and quote by default; only --submit permits remote mutation.\n"
           << "The command reads /dispatch/input/runs/ID/input.tar.zst and writes\n"
           << "/dispatch/output/runs/ID/output.tar.zst. It reports line-oriented progress and\n"
           << "returns zero only after the output and receipt have been published locally.\n"
           << "\n"
           << "Options:\n"
           << "  --policy=cheapest          cheapest, gcp, aws, or azure\n"
           << "  --receipt=FILE             default OUTPUT.receipt\n"
           << "  --cpus=4                   requested virtual CPUs\n"
           << "  --memory-gb=16             requested memory\n"
           << "  --gpu=t4                   optional t4, l4, a10, a100, or h100\n"
           << "  --gpu-count=1              requested accelerators\n"
           << "  --spot                     permit interruptible capacity\n"
           << "  --max-hourly-usd=PRICE     fail closed above this planning estimate\n"
           << "  --retries=1                provider retry count\n"
           << "  --timeout=1h               controller deadline (s, m, or h)\n"
           << "  --expected-runtime=5m      active-runtime cost diagnostic\n"
           << "  --no-catalogue             disable public price lookup\n"
           << "  --allow-unpriced           approve submission when price is unavailable\n"
           << "  --submit                   approve the prepared run and mutate remotely\n"
           << "  --help                     show this text and exit\n"
           << "\n"
           << "Set distinct DISPATCH_INPUT_ROOT and DISPATCH_OUTPUT_ROOT cloud:// bucket/container\n"
           << "roots. Provider infrastructure and\n"
           << "credentials remain in CLDMUX_* configuration. AWS native mounts are CPU-only.\n";
}

options parse_options(int argc, char* argv[]) {
    options result;
    bool id_seen = false;
    bool policy_seen = false;
    bool image_seen = false;
    bool input_seen = false;
    bool output_seen = false;
    bool receipt_seen = false;
    bool cpus_seen = false;
    bool memory_seen = false;
    bool gpu_seen = false;
    bool gpu_count_seen = false;
    bool spot_seen = false;
    bool price_seen = false;
    bool retries_seen = false;
    bool timeout_seen = false;
    bool expected_seen = false;
    bool catalogue_seen = false;
    bool allow_unpriced_seen = false;
    bool submit_seen = false;
    bool command_started = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        std::string_view value;
        if (command_started) {
            result.request.command.emplace_back(argument);
        } else if (argument == "--") {
            command_started = true;
        } else if (argument == "--help") {
            set_once(result.help, "--help");
        } else if (argument == "--spot") {
            set_once(spot_seen, "--spot");
            result.request.spot = true;
        } else if (argument == "--no-catalogue") {
            set_once(catalogue_seen, "--no-catalogue");
            result.request.catalogue_pricing = false;
        } else if (argument == "--allow-unpriced") {
            set_once(allow_unpriced_seen, "--allow-unpriced");
            result.allow_unpriced = true;
        } else if (argument == "--submit") {
            set_once(submit_seen, "--submit");
            result.submit = true;
        } else if (option_value(argument, "--id=", value)) {
            set_once(id_seen, "--id");
            result.request.id = value;
        } else if (option_value(argument, "--policy=", value)) {
            set_once(policy_seen, "--policy");
            result.request.policy = value;
        } else if (option_value(argument, "--image=", value)) {
            set_once(image_seen, "--image");
            result.request.image = value;
        } else if (option_value(argument, "--input=", value)) {
            set_once(input_seen, "--input");
            result.request.input_bundle = value;
        } else if (option_value(argument, "--output=", value)) {
            set_once(output_seen, "--output");
            result.request.output_bundle = value;
        } else if (option_value(argument, "--receipt=", value)) {
            set_once(receipt_seen, "--receipt");
            result.request.receipt_file = value;
        } else if (option_value(argument, "--cpus=", value)) {
            set_once(cpus_seen, "--cpus");
            result.request.cpus = parse_unsigned_option(value, "CPUs", false);
        } else if (option_value(argument, "--memory-gb=", value)) {
            set_once(memory_seen, "--memory-gb");
            result.request.memory_gb = parse_number(value, "memory", false);
        } else if (option_value(argument, "--gpu=", value)) {
            set_once(gpu_seen, "--gpu");
            result.request.gpu = value;
        } else if (option_value(argument, "--gpu-count=", value)) {
            set_once(gpu_count_seen, "--gpu-count");
            result.request.gpu_count = parse_unsigned_option(value, "GPU count", false);
        } else if (option_value(argument, "--max-hourly-usd=", value)) {
            set_once(price_seen, "--max-hourly-usd");
            result.request.max_hourly_usd = parse_number(value, "maximum hourly price", true);
        } else if (option_value(argument, "--retries=", value)) {
            set_once(retries_seen, "--retries");
            result.request.retries = parse_unsigned_option(value, "retries", true);
        } else if (option_value(argument, "--timeout=", value)) {
            set_once(timeout_seen, "--timeout");
            result.request.timeout = parse_duration(value, "timeout");
        } else if (option_value(argument, "--expected-runtime=", value)) {
            set_once(expected_seen, "--expected-runtime");
            result.expected_runtime = parse_duration(value, "expected runtime");
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }

    if (result.help)
        return result;
    if (!command_started || result.request.command.empty())
        throw std::invalid_argument("a command is required after --");
    if (result.expected_runtime > result.request.timeout)
        throw std::invalid_argument("expected runtime must not exceed the controller timeout");
    return result;
}

void show_quote(const options& arguments, const dispatch::prepared_run& prepared) {
    const dispatch::quote& value = prepared.selected_quote();
    record(std::cout, "output_version", "1");
    record(std::cout, "program", "dispatch");
    record(std::cout, "request_id", arguments.request.id);
    record(std::cout, "requested_policy", arguments.request.policy);
    record(std::cout, "provider", value.provider);
    record(std::cout, "region", value.region);
    record(std::cout, "machine", value.machine);
    if (value.hourly_usd) {
        record(std::cout, "hourly_rate_estimate_usd", number(*value.hourly_usd));
        const double hours = static_cast<double>(arguments.expected_runtime.count()) / 3'600'000.0;
        const double estimate = *value.hourly_usd * hours;
        if (!std::isfinite(estimate))
            throw std::runtime_error("expected-runtime cost diagnostic is not finite");
        record(std::cout, "estimated_compute_cost_for_expected_runtime_usd", number(estimate));
    } else {
        record(std::cout, "hourly_rate_estimate_usd", "unavailable");
        record(std::cout, "estimated_compute_cost_for_expected_runtime_usd", "unavailable");
    }
    record_integer(std::cout, "expected_active_runtime_seconds",
                   arguments.expected_runtime.count() / 1'000);
    record_integer(std::cout, "controller_timeout_seconds",
                   arguments.request.timeout.count() / 1'000);
    record_integer(std::cout, "configured_attempt_limit",
                   static_cast<std::uint64_t>(arguments.request.retries) + 1U);
    record(std::cout, "container_input",
           "/dispatch/input/runs/" + arguments.request.id + "/input.tar.zst");
    record(std::cout, "container_output",
           "/dispatch/output/runs/" + arguments.request.id + "/output.tar.zst");
    record(std::cout, "receipt_file", prepared.receipt_path().string());
    for (const auto& warning : value.warnings)
        record(std::cout, "warning", warning);
    record(std::cout, "warning",
           "expected active runtime is caller-supplied; queueing and provisioning are not "
           "predicted");
    if (arguments.request.retries)
        record(std::cout, "warning",
               "configured retries can add cost beyond the single-runtime estimate");
}

void show_receipt(const dispatch::receipt& value) {
    record(std::cout, "execution_id", value.execution_id);
    record(std::cout, "run_id", value.run_id.empty() ? "unavailable" : value.run_id);
    record(std::cout, "job_state", value.job_state);
    record(std::cout, "transaction_phase", value.transaction_phase);
    record(std::cout, "transaction_error",
           value.transaction_error.empty() ? "unavailable" : value.transaction_error);
    record(std::cout, "job_succeeded", value.job_succeeded);
    record(std::cout, "output_retrieved", value.output_retrieved);
    record(std::cout, "receipt_persisted", value.receipt_persisted);
    record(std::cout, "success", value.success);
    if (value.exit_code)
        record_integer(std::cout, "exit_code", *value.exit_code);
    else
        record(std::cout, "exit_code", "unavailable");
    record_integer(std::cout, "elapsed_milliseconds", value.elapsed.count());
    record(std::cout, "message", value.message);
    record(std::cout, "input_sha256",
           value.input_sha256.empty() ? "unavailable" : value.input_sha256);
    record(std::cout, "output_sha256",
           value.output_sha256.empty() ? "unavailable" : value.output_sha256);
    record(std::cout, "remote_output", value.remote_output);
    record(std::cout, "recovery_file", value.recovery_file.string());
    for (const auto& warning : value.warnings)
        record(std::cout, "warning", warning);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const options arguments = parse_options(argc, argv);
        if (arguments.help) {
            print_help(std::cout);
            return 0;
        }

        dispatch::core executor;
        auto prepared = executor.prepare(arguments.request);
        show_quote(arguments, prepared);
        if (!arguments.submit) {
            record(std::cout, "approval", "required");
            record(std::cout, "status", "dry-run");
            return 0;
        }
        if (!prepared.selected_quote().hourly_usd && !arguments.allow_unpriced)
            throw std::runtime_error(
                "hourly price is unavailable; review the diagnostics and pass --allow-unpriced "
                "to approve explicitly");

        record(std::cout, "approval", "--submit");
        record(std::cout, "status", "executing");
        const dispatch::receipt result =
            executor.execute(std::move(prepared),
                             [](std::string_view line) { record(std::cout, "progress", line); });
        show_receipt(result);
        const bool complete = result.success && result.receipt_persisted;
        record(std::cout, "status", complete ? "complete" : "failed");
        return complete ? 0 : 1;
    } catch (const std::exception& failure) {
        record(std::cerr, "error", failure.what());
        return 2;
    }
}
#endif
