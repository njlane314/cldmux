#include "apps/dispatch.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace dispatch {
namespace testing {

std::string sha256_file(const std::filesystem::path& path);
void persist_receipt(receipt value, const std::filesystem::path& path);

} // namespace testing
} // namespace dispatch

namespace {

void check(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

class temporary_directory {
public:
    temporary_directory() {
        std::random_device random;
        const auto root = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            path_ = root / ("cloud-dispatch-test-" + std::to_string(random()) + '-' +
                            std::to_string(attempt));
            std::error_code failure;
            if (std::filesystem::create_directory(path_, failure))
                return;
            if (failure && failure != std::errc::file_exists)
                throw std::runtime_error("cannot create dispatch test directory: " +
                                         failure.message());
        }
        throw std::runtime_error("cannot choose a dispatch test directory");
    }

    temporary_directory(const temporary_directory&) = delete;
    temporary_directory& operator=(const temporary_directory&) = delete;

    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output)
        throw std::runtime_error("cannot write dispatch test file");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read dispatch test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void hash_tests(const std::filesystem::path& directory) {
    const auto artefact = directory / "artefact";

    write_file(artefact, "");
    check(dispatch::testing::sha256_file(artefact) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 empty vector");

    write_file(artefact, "abc");
    check(dispatch::testing::sha256_file(artefact) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 abc vector");

    write_file(artefact, std::string(1'000'000, 'a'));
    check(dispatch::testing::sha256_file(artefact) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "SHA-256 million-a vector");
}

bool valid_key(std::string_view key) {
    if (key.empty() || !((key.front() >= 'a' && key.front() <= 'z') ||
                         (key.front() >= 'A' && key.front() <= 'Z') || key.front() == '_'))
        return false;
    for (const char value : key)
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '_'))
            return false;
    return true;
}

void check_record_grammar(std::string_view text) {
    check(!text.empty() && text.back() == '\n', "receipt has a terminal newline");
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t end = text.find('\n', begin);
        check(end != std::string_view::npos && end != begin, "receipt has complete records");
        const std::string_view line = text.substr(begin, end - begin);
        const std::size_t separator = line.find('=');
        check(separator != std::string_view::npos && valid_key(line.substr(0, separator)),
              "receipt uses KEY=value records");
        for (const char value : line)
            check(static_cast<unsigned char>(value) >= 0x20U && value != 0x7f,
                  "receipt escapes control characters");
        begin = end + 1;
    }
}

void receipt_tests(const std::filesystem::path& directory) {
    dispatch::receipt value;
    value.request_id = "simulation-0042";
    value.execution_id = "execution-9";
    value.run_id = "provider-run-17";
    value.selected.provider = "aws";
    value.selected.region = "eu-west-1";
    value.selected.machine = "FARGATE";
    value.selected.hourly_usd = 0.25;
    value.selected.warnings = {"quote warning"};
    value.success = true;
    value.job_succeeded = true;
    value.output_retrieved = true;
    value.exit_code = 0;
    value.job_state = "succeeded";
    value.transaction_phase = "complete";
    value.transaction_error = "";
    value.message = "line one\nline two\\tail";
    value.elapsed = std::chrono::milliseconds(1'234);
    value.image_reference = "image@sha256:0123";
    value.image_digest = "sha256:0123";
    value.remote_input = "cloud://artefacts/input";
    value.input_size_bytes = 11;
    value.input_sha256 = std::string(64, '1');
    value.input_generation = "input-generation";
    value.input_crc32c = "AAAAAA==";
    value.remote_output = "cloud://artefacts/output";
    value.output_size_bytes = 13;
    value.output_sha256 = std::string(64, '2');
    value.output_generation = "output-generation";
    value.output_crc32c = "AQIDBA==";
    value.recovery_file = directory / "run.receipt.pending.execution-9";
    value.warnings = {"transaction\rwarning"};

    const auto receipt = directory / "run.receipt";
    dispatch::testing::persist_receipt(value, receipt);
    const std::string first = read_file(receipt);
    check_record_grammar(first);
    check(first.find("receipt_version=1\nreceipt_status=complete\n") == 0,
          "receipt declares its schema and completeness");
    check(first.find("run_id=provider-run-17\n") != std::string::npos,
          "receipt records the provider run id");
    check(first.find("execution_id=execution-9\n") != std::string::npos &&
              first.find("transaction_phase=complete\n") != std::string::npos &&
              first.find("transaction_error=unavailable\n") != std::string::npos,
          "receipt separates execution and transaction state");
    check(first.find("recovery_file=" + value.recovery_file.string() + "\n") != std::string::npos,
          "receipt identifies its execution-specific recovery state");
    check(first.find("input_sha256=" + std::string(64, '1') + "\n") != std::string::npos &&
              first.find("output_sha256=" + std::string(64, '2') + "\n") != std::string::npos,
          "receipt records both artefact hashes");
    check(first.find("message=line one\\nline two\\\\tail\n") != std::string::npos,
          "receipt escapes message controls and backslashes");
    check(first.find("warning=quote warning\n") != std::string::npos &&
              first.find("warning=transaction\\rwarning\n") != std::string::npos,
          "receipt preserves repeated escaped warnings");
    check(first.find("receipt_persisted=true\n") != std::string::npos,
          "receipt records successful persistence");

    bool rejected = false;
    try {
        dispatch::testing::persist_receipt(value, receipt);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "receipt publication refuses to overwrite");
    check(read_file(receipt) == first, "failed republication leaves the receipt unchanged");
}

} // namespace

int main() {
    static_assert(std::is_move_constructible_v<dispatch::prepared_run>);
    static_assert(!std::is_copy_constructible_v<dispatch::prepared_run>);
    try {
        const temporary_directory temporary;
        hash_tests(temporary.path());
        receipt_tests(temporary.path());
        std::cout << "PASS  dispatch hashes and receipts\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL  dispatch hashes and receipts\n" << failure.what() << '\n';
        return 1;
    }
}
