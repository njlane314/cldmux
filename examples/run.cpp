#include "cloud.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct options {
    std::string provider = "cheapest";
    bool submit = false;
};

options parse_options(int argc, char* argv[]) {
    options out;
    bool provider_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--submit") {
            if (out.submit)
                throw std::invalid_argument("--submit may be supplied only once");
            out.submit = true;
        } else if (!provider_seen) {
            out.provider = argument;
            provider_seen = true;
        } else {
            throw std::invalid_argument("usage: cloud-run [cheapest|gcp|aws|azure] [--submit]");
        }
    }
    return out;
}

void print_record(std::ostream& output, std::string_view key, std::string_view value) {
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

void print_plan(const cloud::plan& plan) {
    // One KEY=value fact per line keeps output stable for grep, awk, and sed.
    print_record(std::cout, "provider", plan.provider);
    print_record(std::cout, "region", plan.region);
    print_record(std::cout, "machine", plan.machine_type);
    if (plan.estimated_hourly_cost)
        std::cout << "hourly_usd=" << *plan.estimated_hourly_cost << '\n';
    for (const auto& warning : plan.warnings)
        print_record(std::cout, "warning", warning);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const options chosen = parse_options(argc, argv);

        // "cheapest" compares every configured provider. Supplying gcp, aws,
        // or azure is the explicit override; the job itself does not change.
        cloud::client client = cloud::client::from_environment(chosen.provider);

        // Member assignment keeps this portable to C++17. C++20 and newer
        // callers may still use designated initialisers for these aggregates.
        cloud::job_spec job;
        job.name = "hello-cloud";
        job.image = "ubuntu:24.04";
        job.command = {"/bin/echo", "hello"};
        job.resources.cpus = 4;
        job.resources.memory_gb = 16;
        job.resources.gpu_count = 1;
        job.resources.spot = false;
        job.resources.max_price_per_hour = std::nullopt;
        job.retries = 1;
        job.auto_delete = true;
        job.timeout = std::chrono::minutes(15);

        // route(job) compares prices once, then returns a cheap client pinned to
        // that winner. Planning again prints a fresh quote for the same provider;
        // the subsequent run cannot silently switch to another cloud.
        cloud::client routed = client.route(job);
        print_plan(routed.plan(job));
        if (!chosen.submit) {
            print_record(std::cout, "status", "dry-run");
            return 0;
        }

        const cloud::result result = routed.run(job).wait(
            [](const cloud::log_entry& line) { print_record(std::cout, "log", line.text); });
        if (!result.success()) {
            print_record(std::cerr, "error", result.error());
            return 1;
        }
        return 0;
    } catch (const std::exception& failure) {
        print_record(std::cerr, "error", failure.what());
        return 2;
    }
}
