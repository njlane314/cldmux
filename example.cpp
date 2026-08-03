#if defined(CLOUD_TEST_AMALGAMATED)
#include "cloud.h"
#else
#include <cloud/cloud.hpp>
#endif

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// This is the repository's only example programme. It is both a runnable,
// provider-neutral command and a commented checklist of the choices an author
// makes when adopting the library. The active path stays deliberately small:
// diagnose a portable container job, print grep-friendly output, and submit it
// only after the caller supplies --submit.
namespace {

struct options {
    std::string provider = "cheapest";
    std::chrono::milliseconds expected_attempt_runtime{std::chrono::minutes(5)};
    bool estimate = false;
    bool submit = false;
    bool help = false;
};

void print_help(std::ostream& stream, std::string_view program) {
    stream << "Usage: " << program << " [cheapest|gcp|aws|azure] [OPTIONS]\n"
           << "\n"
           << "Provider choice:\n"
           << "  cheapest   compare every configured provider and route to the lowest cost\n"
           << "  gcp        override routing and use GCP\n"
           << "  aws        override routing and use AWS\n"
           << "  azure      override routing and use Azure\n"
           << "\n"
           << "Options:\n"
           << "  --expected-attempt-runtime=5m  cost sensitivity for one attempt (s, m, h)\n"
           << "  --estimate                     price an explicit provider (cheapest already does)\n"
           << "  --submit                       permit billable storage and job operations\n"
           << "  --help                         show these choices and exit\n"
           << "\n"
           << "The default is a read-only cheapest-provider diagnostic. Provider credentials,\n"
           << "locations, queues, endpoints, and native storage resources use CLOUD_* and\n"
           << "provider credential variables, or an explicit cloud::config in this source.\n";
}

// This parser is application policy rather than cloud API. Keeping it here
// makes the example genuinely single-file and keeps the public library free of
// opinions about a programme's command-line interface.
std::chrono::milliseconds parse_runtime(std::string_view text) {
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

bool is_provider(std::string_view value) {
    return value == "cheapest" || value == "gcp" || value == "aws" || value == "azure";
}

options parse_options(int argc, char* argv[]) {
    options out;
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
        } else if (argument == "--help") {
            if (out.help)
                throw std::invalid_argument("--help may be supplied only once");
            out.help = true;
        } else if (argument.substr(0, runtime_option.size()) == runtime_option) {
            if (runtime_seen)
                throw std::invalid_argument(
                    "--expected-attempt-runtime may be supplied only once");
            out.expected_attempt_runtime = parse_runtime(argument.substr(runtime_option.size()));
            runtime_seen = true;
        } else if (argument.substr(0, 2) == "--") {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        } else if (!provider_seen) {
            if (!is_provider(argument))
                throw std::invalid_argument(
                    "provider must be cheapest, gcp, aws, or azure: " + std::string(argument));
            out.provider = argument;
            provider_seen = true;
        } else {
            throw std::invalid_argument(
                "usage: cloud-run [cheapest|gcp|aws|azure] "
                "[--expected-attempt-runtime=5m] [--estimate] [--submit] [--help]");
        }
    }
    return out;
}

cloud::client make_client(const options& chosen) {
    // "cheapest" automatically enables public-catalogue prices and requires at
    // least two configured providers. For an explicit provider, --estimate is
    // the opt-in to read-only catalogue network requests; without it, planning
    // stays local and the diagnostic reports an unavailable price.
    if (chosen.estimate)
        return cloud::client::from_environment(chosen.provider,
                                               cloud::price_source::public_catalogue);
    return cloud::client::from_environment(chosen.provider);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const options chosen = parse_options(argc, argv);
        if (chosen.help) {
            print_help(std::cout, "cloud-run");
            return 0;
        }

        // CONFIGURATION CHOICE
        //
        // from_environment() keeps deploy-time values outside the programme in
        // grep-friendly KEY=value variables. Use an explicit cloud::config
        // instead when the programme must provide several provider-specific
        // regions, AWS GPU queues, storage mappings, logical VM templates,
        // private endpoints, credential callbacks, price callbacks, or custom
        // polling and timeout values. Explicit configuration also offers
        // selection::ordered as an alternative to selection::lowest_cost. Both
        // configuration forms produce the same client API. supports() can check
        // implemented features, while plan() validates one concrete job against
        // the selected configuration, account, and provider restrictions.
        cloud::client router = make_client(chosen);

        // WORKLOAD CHOICE
        //
        // job_spec is the provider-neutral description passed to plan(),
        // diagnose(), route(), and run(). Member assignment keeps this example
        // valid in C++17; C++20 and newer callers may use designated initialisers.
        cloud::job_spec job;
        job.name = "hello-cloud";
        job.image = "ubuntu:24.04";

        // Commands are shell-free tokens. GCP treats the first token as the
        // entrypoint; AWS and Azure pass all tokens to the image entrypoint.
        // Choose an image whose ENTRYPOINT matches that cross-provider contract.
        job.command = {"/bin/echo", "hello"};

        // Leave workdir empty to use the image default, or set an absolute
        // container path. workdir does not upload local source code.
        job.workdir.clear();

        // Leave service_account empty for provider defaults. A non-empty value
        // maps to a GCP VM service-account email or AWS job role; Azure rejects
        // it because its auto-pool has no equivalent job field.
        job.service_account.clear();

        // MOUNT CHOICE
        //
        // The portable base job has no mounts. Authors may add entries such as:
        // job.mounts = {{"cloud://input-bucket/", "/input", true}};
        // GCP supports a bucket or prefix, AWS S3 Files requires Fargate and
        // rejects mounted GPU jobs, and Azure requires a complete Blob container.
        job.mounts.clear();

        // RESOURCE CHOICE
        //
        // CPU and memory are portable minimums which planning rounds up to a
        // supported native shape. Leave gpu empty for CPU-only work; otherwise
        // choose t4, l4, a10, a100, or h100 and set gpu_count. Planning fails
        // rather than silently substituting a different accelerator.
        job.resources.cpus = 4;
        job.resources.memory_gb = 16;
        job.resources.gpu.clear();
        job.resources.gpu_count = 1;

        // Spot can reduce price but permits interruption. A price ceiling is an
        // optional fail-closed guard: setting it requires an available estimate.
        job.resources.spot = false;
        job.resources.max_price_per_hour = std::nullopt;

        // LIFECYCLE CHOICE
        //
        // retries counts attempts after the first. timeout is the live
        // controller deadline and maps to provider attempt limits; queueing,
        // recovery, cancellation, and cleanup can extend real elapsed time.
        // auto_delete removes controller-owned resources as far as each API
        // permits. Set it false only when retained provider records are useful.
        job.retries = 1;
        job.timeout = std::chrono::minutes(15);
        job.auto_delete = true;

        // ROUTING AND COST CHOICE
        //
        // plan() is the smallest read-only shape check. diagnose() additionally
        // chooses a route and multiplies an available hourly estimate by the
        // caller-modelled active time for one attempt. It is a sensitivity, not
        // a runtime prediction, reservation, or bill. route() then binds every
        // provider-owned action to the visible winner so later storage and
        // submission cannot switch. apps/empirical.cpp demonstrates a separate
        // policy which combines routing-time quote snapshots with observed
        // workload runtimes.
        const cloud::run_diagnostics report =
            router.diagnose(job, chosen.expected_attempt_runtime);
        cloud::client client = router.route(report.selected_plan.provider);

        // OUTPUT CHOICE
        //
        // The library provides ordered, escaped KEY=value diagnostics. An
        // application can add records, set or erase standard keys, rename them,
        // inspect records(), and choose stdout, stderr, or a file destination.
        cloud::command_output output =
            cloud::command_output::diagnostics(chosen.provider, job, report);
        output.add("program", "cloud-run");
        output.write(std::cout);

        // This is the safety boundary. Planning and catalogue lookup are
        // read-only; object writes, raw-compute controls, and run() belong below
        // the explicit --submit decision because they may incur charges.
        if (!chosen.submit) {
            cloud::write_command_record(std::cout, "status", "dry-run");
            return 0;
        }

        try {
            // DATA CHOICE
            //
            // A bound client's storage() maps cloud://bucket/key to GCS, S3, or
            // Blob Storage. Optional preparation and collection calls include:
            // client.storage().put_file("cloud://inputs/data", "./data");
            // client.storage().get_file("cloud://outputs/result", "./result");
            // list(), stat(), get(), put(), and remove() use the same route.
            //
            // RAW-COMPUTE CHOICE
            //
            // Batch is preferable when the provider should queue and clean up a
            // container job. For long-lived native VMs, use the separate facade:
            // client.compute().create("worker", "logical-template").wait();
            // instances(), start(), stop(), and destroy() remain route-bound.

            cloud::write_command_record(std::cout, "status", "submitting");
            std::cout.flush();
            const cloud::job submitted = client.run(job);
            cloud::write_command_record(std::cout, "job_id", submitted.id());

            // wait() owns polling, line-oriented native logs, cancellation at the
            // controller deadline, final log draining, and configured cleanup.
            // status(), logs(), and cancel() support a custom controller. Copies
            // of one job share state, so serialise lifecycle method calls.
            const cloud::result result = submitted.wait([](const cloud::log_entry& line) {
                cloud::write_command_record(std::cout, "log", line.text);
            });
            cloud::command_output::job_result(result).write(std::cout);
            if (!result.success()) {
                cloud::write_command_record(std::cerr, "error", result.error());
                cloud::write_command_record(std::cout, "status", "failed");
                return 1;
            }

            cloud::write_command_record(std::cout, "status", "succeeded");
            return 0;
        } catch (...) {
            cloud::write_command_record(std::cout, "status", "failed");
            throw;
        }
    } catch (const std::exception& failure) {
        // Even errors use the library's one-record escaping, so hostile values
        // cannot inject extra diagnostic lines.
        cloud::write_command_record(std::cerr, "error", failure.what());
        return 2;
    }
}
