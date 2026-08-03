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

void print_plan(const cloud::plan& plan) {
    std::cout << "provider: " << plan.provider << '\n'
              << "region:   " << plan.region << '\n'
              << "machine:  " << plan.machine_type << '\n';
    if (plan.estimated_hourly_cost)
        std::cout << "price:    $" << *plan.estimated_hourly_cost << " per hour\n";
    for (const auto& warning : plan.warnings)
        std::cout << "warning:  " << warning << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const options chosen = parse_options(argc, argv);

        // "cheapest" compares every configured provider. Supplying gcp, aws,
        // or azure is the explicit override; the job itself does not change.
        cloud::client client = cloud::client::from_environment(chosen.provider);
        const cloud::job_spec job{
            .name = "hello-cloud",
            .image = "ubuntu:24.04",
            .command = {"/bin/echo", "hello"},
            .workdir = {},
            .service_account = {},
            .mounts = {},
            .resources =
                {
                    .cpus = 4,
                    .memory_gb = 16,
                    .gpu = {},
                    .gpu_count = 1,
                    .spot = false,
                    .max_price_per_hour = std::nullopt,
                },
            .retries = 1,
            .auto_delete = true,
            .timeout = std::chrono::minutes(15),
        };

        print_plan(client.plan(job));
        if (!chosen.submit) {
            std::cout << "dry run only; pass --submit to run this job\n";
            return 0;
        }

        const cloud::result result = client.run(job).wait(
            [](const cloud::log_entry& line) { std::cout << line.text << '\n'; });
        if (!result.success()) {
            std::cerr << "job failed: " << result.error() << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "cloud-run: " << failure.what() << '\n';
        return 2;
    }
}
