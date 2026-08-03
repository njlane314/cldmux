#include "burst.hpp"

#include <cloud>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace burst {
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
    std::string input = environment("BURST_INPUT_ROOT");
    std::string output = environment("BURST_OUTPUT_ROOT");
    if (input.empty() || output.empty())
        throw std::invalid_argument(
            "distinct BURST_INPUT_ROOT and BURST_OUTPUT_ROOT values are required");
    roots result{storage_root(std::move(input), "BURST_INPUT_ROOT"),
                 storage_root(std::move(output), "BURST_OUTPUT_ROOT")};
    if (result.input == result.output)
        throw std::invalid_argument(
            "BURST_INPUT_ROOT and BURST_OUTPUT_ROOT must name different buckets or containers");
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
    return result;
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
    if (input == output || input == receipt_path || output == receipt_path ||
        input == pending_path || output == pending_path)
        throw std::invalid_argument(
            "input, output, receipt, and pending receipt paths must be distinct");
    if (std::filesystem::exists(output))
        throw std::invalid_argument("output bundle already exists");
    if (std::filesystem::exists(receipt_path))
        throw std::invalid_argument("receipt already exists");
    if (std::filesystem::exists(pending_path))
        throw std::invalid_argument("pending burst receipt already exists");

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

quote public_quote(const cloud::plan& value) {
    return {value.provider, value.region, value.machine_type, value.estimated_hourly_cost,
            value.warnings};
}

std::string cloud_state(cloud::job_state value) {
    switch (value) {
    case cloud::job_state::queued:
        return "queued";
    case cloud::job_state::scheduled:
        return "scheduled";
    case cloud::job_state::running:
        return "running";
    case cloud::job_state::succeeded:
        return "succeeded";
    case cloud::job_state::failed:
        return "failed";
    case cloud::job_state::cancelling:
        return "cancelling";
    case cloud::job_state::cancelled:
        return "cancelled";
    case cloud::job_state::deleting:
        return "deleting";
    case cloud::job_state::unknown:
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
                                   ('.' + destination.filename().string() + ".burst-" +
                                    std::string(purpose) + '-' + suffix.str() + ".tmpdir");
            std::error_code failure;
            if (!std::filesystem::create_directory(candidate, failure)) {
                if (!failure || failure == std::errc::file_exists)
                    continue;
                throw std::runtime_error("cannot create private temporary directory: " +
                                         failure.message());
            }
            directory_ = candidate;
            std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, failure);
            if (failure) {
                std::error_code ignored;
                std::filesystem::remove(directory_, ignored);
                throw std::runtime_error("cannot protect private temporary directory: " +
                                         failure.message());
            }
            path_ = directory_ / "content";
            return;
        }
        throw std::runtime_error("cannot choose a private temporary artefact directory");
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

cloud::command_output receipt_records(const receipt& value, std::string_view status) {
    cloud::command_output output;
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
        std::error_code failure;
        std::filesystem::rename(temporary.path(), state_path_, failure);
        if (failure)
            throw std::runtime_error("cannot update pending receipt: " + failure.message());
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

bool object_exists(const cloud::storage& storage, std::string_view uri) {
    try {
        (void)storage.stat(uri);
        return true;
    } catch (const cloud::error& failure) {
        if (failure.http_status() == 404)
            return false;
        throw;
    }
}

bool same_object(const cloud::object& first, const cloud::object& second) {
    return !first.generation.empty() && first.generation == second.generation &&
           first.size == second.size && first.crc32c == second.crc32c;
}

} // namespace

struct prepared_run::implementation {
    implementation(request requested_value, cloud::job_spec job_value,
                   cloud::client bound_value, cloud::plan selected_value,
                   quote quoted_value, std::string input_value, std::string output_value,
                   std::filesystem::path pending_value)
        : requested(std::move(requested_value)), job(std::move(job_value)),
          bound(std::move(bound_value)), selected(std::move(selected_value)),
          quoted(std::move(quoted_value)), input_uri(std::move(input_value)),
          output_uri(std::move(output_value)), pending_file(std::move(pending_value)) {}

    request requested;
    cloud::job_spec job;
    cloud::client bound;
    cloud::plan selected;
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

    cloud::job_spec job;
    job.name = "burst-" + value.id;
    job.image = value.image;
    job.command = value.command;
    job.mounts = {{artefact_roots.input, "/burst/input", true},
                  {artefact_roots.output, "/burst/output", false}};
    job.resources.cpus = value.cpus;
    job.resources.memory_gb = value.memory_gb;
    job.resources.gpu = value.gpu;
    job.resources.gpu_count = value.gpu_count;
    job.resources.spot = value.spot;
    job.resources.max_price_per_hour = value.max_hourly_usd;
    job.retries = value.retries;
    job.timeout = value.timeout;
    job.auto_delete = true;

    cloud::client router =
        value.catalogue_pricing
            ? cloud::client::from_environment(value.policy, cloud::price_source::public_catalogue)
            : cloud::client::from_environment(value.policy);
    cloud::plan selected = router.plan(job);
    cloud::client bound = router.route(selected.provider);
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
    std::optional<cloud::job> submitted;
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
    if (value.recovery_file == state->requested.input_bundle ||
        value.recovery_file == state->requested.output_bundle ||
        value.recovery_file == state->requested.receipt_file)
        throw std::invalid_argument("recovery state path collides with a transaction path");
    // Refresh against the approved quote ceiling before any remote mutation.
    const cloud::plan refreshed = state->bound.plan(state->job);
    if (refreshed.provider != state->selected.provider)
        throw std::runtime_error("provider-bound refresh changed provider");
    const cloud::storage storage = state->bound.storage();
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
        cloud::put_options create_only;
        create_only.if_generation_match = "0";
        const cloud::object uploaded =
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
        const cloud::result result =
            submitted->wait([&](const cloud::log_entry& entry) { report_progress(entry.text); });
        value.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - *started);
        terminal_result = true;
        value.job_state = cloud_state(result.state);
        value.job_succeeded = result.success();
        value.exit_code = result.exit_code;
        value.message = result.message;
        value.warnings.insert(value.warnings.end(), result.warnings.begin(), result.warnings.end());
        value.transaction_phase = "terminal";
        lease.update(value, "terminal");

        try {
            const cloud::object current_input = storage.stat(state->input_uri);
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
            const cloud::object before = storage.stat(state->output_uri);
            temporary_file downloaded(state->requested.output_bundle, "output");
            storage.get_file(state->output_uri, downloaded.path());
            const cloud::object after = storage.stat(state->output_uri);
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

#if defined(BURST_TESTING)
namespace testing {

std::string sha256_file(const std::filesystem::path& path) { return fingerprint_file(path).sha256; }

void persist_receipt(receipt value, const std::filesystem::path& path) {
    value.receipt_file = path;
    value.receipt_persisted = true;
    persist_final(value, "complete");
}

} // namespace testing
#endif

} // namespace burst
