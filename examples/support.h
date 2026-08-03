#ifndef CLOUD_EXAMPLE_SUPPORT_H_INCLUDED
#define CLOUD_EXAMPLE_SUPPORT_H_INCLUDED

#if defined(CLOUD_TEST_AMALGAMATED)
#include "cloud.h"
#else
#include <cloud/cloud.hpp>
#endif

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
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

} // namespace cloud_example

#endif
