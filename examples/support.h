#ifndef CLOUD_EXAMPLE_SUPPORT_H_INCLUDED
#define CLOUD_EXAMPLE_SUPPORT_H_INCLUDED

#include "cloud.h"

#include <charconv>
#include <chrono>
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

namespace cloud_example {

struct options {
    std::string provider;
    std::chrono::milliseconds expected_attempt_runtime{};
    bool estimate = false;
    bool submit = false;
};

inline std::chrono::milliseconds parse_runtime(std::string_view text) {
    if (text.size() < 2)
        throw std::invalid_argument(
            "expected attempt runtime requires a positive s, m, or h suffix");

    const char suffix = text.back();
    std::uint64_t multiplier = 0;
    if (suffix == 's')
        multiplier = 1'000;
    else if (suffix == 'm')
        multiplier = 60'000;
    else if (suffix == 'h')
        multiplier = 3'600'000;
    else
        throw std::invalid_argument("expected attempt runtime requires an s, m, or h suffix");

    std::uint64_t quantity = 0;
    const std::string_view number = text.substr(0, text.size() - 1);
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), quantity);
    using rep = std::chrono::milliseconds::rep;
    const auto limit = static_cast<std::uint64_t>((std::numeric_limits<rep>::max)());
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || !quantity ||
        quantity > limit / multiplier)
        throw std::invalid_argument(
            "expected attempt runtime must be a positive, representable duration");
    return std::chrono::milliseconds(static_cast<rep>(quantity * multiplier));
}

inline options parse_options(int argc, char* argv[], std::string default_provider,
                             std::chrono::milliseconds default_runtime,
                             std::string_view program_name) {
    options out;
    out.provider = std::move(default_provider);
    out.expected_attempt_runtime = default_runtime;
    bool provider_seen = false;
    bool runtime_seen = false;
    constexpr std::string_view runtime_option = "--expected-attempt-runtime=";
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--submit") {
            if (out.submit)
                throw std::invalid_argument("--submit may be supplied only once");
            out.submit = true;
        } else if (argument == "--estimate") {
            if (out.estimate)
                throw std::invalid_argument("--estimate may be supplied only once");
            out.estimate = true;
        } else if (argument.substr(0, runtime_option.size()) == runtime_option) {
            if (runtime_seen)
                throw std::invalid_argument(
                    "--expected-attempt-runtime may be supplied only once");
            out.expected_attempt_runtime = parse_runtime(argument.substr(runtime_option.size()));
            runtime_seen = true;
        } else if (argument.substr(0, 2) == "--") {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        } else if (!provider_seen) {
            out.provider = argument;
            provider_seen = true;
        } else {
            throw std::invalid_argument(
                "usage: " + std::string(program_name) +
                " [cheapest|gcp|aws|azure] [--expected-attempt-runtime=5m] [--estimate] "
                "[--submit]");
        }
    }
    return out;
}

inline cloud::client make_client(const options& chosen) {
    if (chosen.estimate)
        return cloud::client::from_environment(chosen.provider,
                                               cloud::price_source::public_catalogue);
    return cloud::client::from_environment(chosen.provider);
}

inline void print_record(std::ostream& output, std::string_view key, std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    output << key << '=';
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '\\')
            output << "\\\\";
        else if (c == '\n')
            output << "\\n";
        else if (c == '\r')
            output << "\\r";
        else if (c == '\t')
            output << "\\t";
        else if (c < 0x20 || c == 0x7f)
            output << "\\x" << hex[c >> 4] << hex[c & 0x0f];
        else
            output << raw;
    }
    output << '\n';
}

inline void print_number(std::ostream& output, std::string_view key, double value) {
    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::setprecision(12) << value;
    print_record(output, key, formatted.str());
}

inline void print_duration_seconds(std::ostream& output, std::string_view key,
                                   std::chrono::milliseconds value) {
    print_number(output, key, std::chrono::duration<double>(value).count());
}

inline void print_cost(std::ostream& output, std::string_view key,
                       const std::optional<double>& value) {
    if (value)
        print_number(output, key, *value);
    else
        print_record(output, key, "unavailable");
}

inline std::string_view job_state_name(cloud::job_state state) {
    switch (state) {
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

inline void print_result(const cloud::result& result) {
    print_record(std::cout, "job_state", job_state_name(result.state));
    if (result.exit_code)
        print_record(std::cout, "exit_code", std::to_string(*result.exit_code));
    for (const auto& warning : result.warnings)
        print_record(std::cout, "warning", warning);
}

inline void print_diagnostics(const options& chosen, const cloud::job_spec& job,
                              const cloud::run_diagnostics& report) {
    print_record(std::cout, "output_version", "1");
    print_record(std::cout, "requested_provider", chosen.provider);
    print_record(std::cout, "job_name", job.name);
    print_record(std::cout, "provider", report.selected_plan.provider);
    print_record(std::cout, "region", report.selected_plan.region);
    print_record(std::cout, "machine", report.selected_plan.machine_type);
    print_record(std::cout, "requested_cpus", std::to_string(job.resources.cpus));
    print_number(std::cout, "requested_memory_gb", job.resources.memory_gb);
    print_record(std::cout, "accelerator",
                 report.selected_plan.accelerator.empty() ? "none"
                                                          : report.selected_plan.accelerator);
    print_record(std::cout, "accelerator_count",
                 std::to_string(report.selected_plan.accelerator_count));
    print_record(std::cout, "spot", job.resources.spot ? "true" : "false");
    print_duration_seconds(std::cout, "expected_attempt_runtime_seconds",
                           report.expected_attempt_runtime);
    print_duration_seconds(std::cout, "controller_timeout_seconds",
                           report.controller_timeout);
    print_record(std::cout, "provider_attempt_timeout_seconds",
                 std::to_string(report.provider_attempt_timeout.count()));
    print_record(std::cout, "provider_job_timeout_seconds",
                 report.provider_job_timeout ? std::to_string(report.provider_job_timeout->count())
                                             : "not-applicable");
    print_record(std::cout, "configured_retries", std::to_string(report.configured_retries));
    print_record(std::cout, "configured_attempt_limit",
                 std::to_string(report.configured_attempt_limit));
    print_record(std::cout, "cost_currency", "USD");
    print_cost(std::cout, "hourly_rate_estimate_usd",
               report.selected_plan.estimated_hourly_cost);
    print_cost(std::cout, "estimated_cost_for_expected_attempt_runtime_usd",
               report.estimated_cost_for_expected_attempt_runtime);
    print_record(std::cout, "estimate_basis", "expected-attempt-runtime-times-hourly-rate");
    for (const auto& warning : report.warnings)
        print_record(std::cout, "warning", warning);
}

} // namespace cloud_example

#endif
