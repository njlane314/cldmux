#include "cloud.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// The program stays provider-neutral. Choosing a provider is a runtime option,
// so the job, storage calls, and lifecycle code below never branch on gcp/aws/
// azure. "cheapest" enables the internal router; a provider name overrides it.
struct options {
    std::string provider = "gcp";
    bool submit = false;
};

options parse_options(int argc, char* argv[]) {
    options result;
    bool provider_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--submit") {
            if (result.submit)
                throw std::invalid_argument("--submit may be supplied only once");
            result.submit = true;
        } else if (!provider_seen) {
            result.provider = std::string(argument);
            provider_seen = true;
        } else {
            throw std::invalid_argument(
                "usage: example [cheapest|gcp|aws|azure] [--submit]");
        }
    }
    return result;
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

} // namespace

int main(int argc, char* argv[]) {
    try {
        const options chosen = parse_options(argc, argv);

        // from_environment() reads plain KEY=value configuration. Credentials,
        // queues, Batch endpoints, regions, and native mount resources therefore
        // remain outside this source file. The default is GCP only so a first dry
        // run needs one provider; pass "cheapest" after configuring at least two.
        cloud::client router = cloud::client::from_environment(chosen.provider);

        // job_spec is the provider-independent description of one invocation.
        // Member assignment (rather than designated initialisers) keeps this
        // example valid in C++17 as well as C++20, C++23, and C++26 modes.
        cloud::job_spec spec;
        spec.name = "simulation-42";
        spec.image = "ghcr.io/example/simulation:latest";

        // Commands are shell-free tokens. The illustrative input is a small
        // KEY=value file, and the result is line-oriented text; both remain easy
        // to inspect with grep and ordinary tools.
        spec.command = {"/usr/local/bin/simulate", "--config", "/input/parameters.conf",
                        "--output", "/output/result.txt"};

        // A mount names a storage collection rather than one object. Bucket or
        // container roots are the portable intersection: GCP may additionally
        // mount a prefix, while AWS and Azure apply the native restrictions
        // documented in the README. The input collection is read-only.
        spec.mounts = {
            {"cloud://sim-input", "/input", true},
            {"cloud://sim-output", "/output", false},
        };

        // The planner rounds these portable minimums up to a supported native
        // shape. Set gpu to t4, l4, a10, a100, or h100 when the configured route
        // supports that exact model. AWS mounted jobs deliberately reject GPUs
        // because S3 Files volumes are Fargate-only.
        spec.resources.cpus = 4;
        spec.resources.memory_gb = 16;
        spec.resources.gpu_count = 1;
        spec.resources.spot = false;

        // A maximum price fails closed when no comparable estimate is available.
        // It is omitted here so an explicit provider can plan without a network
        // price lookup. "cheapest" enables the built-in catalogue automatically.
        spec.resources.max_price_per_hour = std::nullopt;

        // Providers express retry limits differently, but the portable value is
        // always the number of retries after the first attempt. auto_delete
        // cleans up controller-owned resources as far as each API permits.
        spec.retries = 2;
        spec.auto_delete = true;
        spec.timeout = std::chrono::hours(2);

        // route(spec) chooses once and returns a cheap client bound to the winner.
        // Every later storage, compute, planning, and submission call therefore
        // stays on that provider. Passing gcp/aws/azure above is the override.
        cloud::client client = router.route(spec);
        const cloud::plan plan = client.plan(spec);
        // Stable KEY=value records are intentionally friendly to grep, awk,
        // sed, and shell scripts.
        print_record(std::cout, "provider", plan.provider);
        print_record(std::cout, "region", plan.region);
        print_record(std::cout, "machine", plan.machine_type);
        if (plan.estimated_hourly_cost)
            std::cout << "hourly_usd=" << *plan.estimated_hourly_cost << '\n';
        for (const auto& warning : plan.warnings)
            print_record(std::cout, "warning", warning);

        // Planning may perform read-only catalogue requests, but it never
        // allocates compute. Nothing below runs without the explicit --submit
        // flag because uploads and jobs can consume billable cloud resources.
        if (!chosen.submit) {
            print_record(std::cout, "status", "dry-run");
            return 0;
        }

        // cloud:// is resolved by the route: GCS on GCP, S3 on AWS, and Blob
        // Storage on Azure. Uploading before run() makes the same object visible
        // through the input mount without provider-specific application code.
        client.storage().put_file("cloud://sim-input/parameters.conf", "./parameters.conf");

        // The returned job handle owns provider-native polling, cancellation,
        // final log draining, and cleanup. Logging is delivered one line at a
        // time; it is intentionally not presented as a live byte stream.
        const cloud::result result = client.run(spec).wait([](const cloud::log_entry& line) {
            print_record(std::cout, "log", line.text);
        });
        if (!result.success()) {
            print_record(std::cerr, "error", result.error());
            return 1;
        }

        // get_file() downloads to a temporary file, verifies library CRC32C
        // metadata when present, and then replaces the destination. The calling
        // code remains identical for every route.
        client.storage().get_file("cloud://sim-output/result.txt", "./result.txt");
        return 0;
    } catch (const std::exception& failure) {
        print_record(std::cerr, "error", failure.what());
        return 2;
    }
}
