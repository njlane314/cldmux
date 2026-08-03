#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace burst {

// One already-archived workload. Infrastructure and credentials stay outside
// this domain-facing type; policy is the only routing choice the caller makes.
struct request {
    std::string id;
    std::string policy = "cheapest"; // cheapest | gcp | aws | azure

    std::string image;
    std::vector<std::string> command;

    std::filesystem::path input_bundle;
    std::filesystem::path output_bundle;
    // An empty path becomes output_bundle + ".receipt" during prepare().
    std::filesystem::path receipt_file;

    unsigned cpus = 4;
    double memory_gb = 16;
    std::string gpu; // "", t4, l4, a10, a100, h100
    unsigned gpu_count = 1;
    bool spot = false;

    std::optional<double> max_hourly_usd;
    unsigned retries = 1;
    std::chrono::milliseconds timeout = std::chrono::hours(1);
    // Catalogue lookup is read-only but may perform network requests.
    bool catalogue_pricing = true;
};

struct quote {
    std::string provider;
    std::string region;
    std::string machine;
    std::optional<double> hourly_usd;
    std::vector<std::string> warnings;
};

// A receipt distinguishes the provider job from the complete artefact
// transaction. Hashes are lowercase SHA-256; empty metadata is unavailable.
struct receipt {
    std::string request_id;
    std::string execution_id;
    std::string run_id;
    quote selected;

    bool success = false;
    bool job_succeeded = false;
    bool output_retrieved = false;
    bool receipt_persisted = false;
    std::optional<int> exit_code;
    std::string job_state = "not-submitted";
    std::string transaction_phase = "not-started";
    std::string transaction_error;
    std::string message;
    std::chrono::milliseconds elapsed{};

    std::string image_reference;
    std::string image_digest;

    std::string remote_input;
    std::uint64_t input_size_bytes = 0;
    std::string input_sha256;
    std::string input_generation;
    std::string input_crc32c;

    std::string remote_output;
    std::uint64_t output_size_bytes = 0;
    std::string output_sha256;
    std::string output_generation;
    std::string output_crc32c;

    std::filesystem::path receipt_file;
    std::filesystem::path recovery_file;
    std::vector<std::string> warnings;
};

using progress = std::function<void(std::string_view)>;

class core;

// The implementation owns a provider-bound client. Moving this value transfers
// the one right to execute it; copying could accidentally submit twice.
class prepared_run {
public:
    prepared_run(prepared_run&&) noexcept;
    prepared_run& operator=(prepared_run&&) noexcept;
    prepared_run(const prepared_run&) = delete;
    prepared_run& operator=(const prepared_run&) = delete;
    ~prepared_run();

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const quote& selected_quote() const;
    [[nodiscard]] const std::filesystem::path& receipt_path() const;

private:
    struct implementation;

    explicit prepared_run(std::unique_ptr<implementation> value) noexcept;
    std::unique_ptr<implementation> implementation_;

    friend class core;
};

class core {
public:
    // Read-only apart from catalogue queries: validate, plan once, then pin.
    [[nodiscard]] prepared_run prepare(request value) const;

    // The caller approves a prepared quote by invoking execute().
    [[nodiscard]] receipt execute(prepared_run prepared, progress on_progress = {}) const;
};

} // namespace burst
