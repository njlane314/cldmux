#include "dispatch.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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
