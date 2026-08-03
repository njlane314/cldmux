#include "examples/support.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>

int main(int argc, char* argv[]) {
    try {
        // The program stays provider-neutral. Choosing a provider is a runtime
        // option, so the job, storage calls, and lifecycle code never branch on
        // gcp/aws/azure. "cheapest" enables the internal router; a provider name
        // overrides it. The caller supplies the per-attempt runtime expectation
        // used only for the advisory cost calculation.
        const cloud_example::options chosen = cloud_example::parse_options(
            argc, argv, "gcp", std::chrono::minutes(90), "example");

        // from_environment() reads plain KEY=value configuration. Credentials,
        // queues, Batch endpoints, regions, and native mount resources therefore
        // remain outside this source file. The default is GCP only so a first dry
        // run needs one provider; pass "cheapest" after configuring at least two.
        cloud::client router = cloud_example::make_client(chosen);

        // job_spec is the provider-independent description of one invocation.
        // Member assignment (rather than designated initialisers) keeps this
        // example valid in C++17 as well as C++20 and C++23 modes.
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

        // diagnose() performs the read-only routing decision once and adds
        // retry, timeout, and compute-cost facts. The selected provider is then
        // bound explicitly so storage and submission match the visible report.
        // Passing --expected-attempt-runtime=45m changes only the caller's
        // estimate; spec.timeout remains the enforced controller/provider limit.
        const cloud::run_diagnostics report =
            router.diagnose(spec, chosen.expected_attempt_runtime);
        cloud::client client = router.route(report.selected_plan.provider);

        // The cloud library owns the stable KEY=value syntax, including validation,
        // control-character escaping, and locale-independent numbers. The
        // returned record set is deliberately mutable: this program adds an
        // application label, and could use set(), rename(), or erase() to adapt
        // standard fields before choosing stdout, stderr, or a file.
        cloud::command_output output =
            cloud::command_output::diagnostics(chosen.provider, spec, report);
        output.add("application", "simulation");
        output.write(std::cout);

        // Planning may perform read-only catalogue requests, but it never
        // allocates compute. Nothing below runs without the explicit --submit
        // flag because uploads and jobs can consume billable cloud resources.
        if (!chosen.submit) {
            cloud::write_command_record(std::cout, "status", "dry-run");
            return 0;
        }

        try {
            // Flush the complete preflight report before the first mutation. This
            // makes it available even if a later upload or submission fails.
            cloud::write_command_record(std::cout, "status", "preparing");
            std::cout.flush();

            // cloud:// is resolved by the route: GCS on GCP, S3 on AWS, and Blob
            // Storage on Azure. Uploading before run() makes the same object visible
            // through the input mount without provider-specific application code.
            client.storage().put_file("cloud://sim-input/parameters.conf", "./parameters.conf");

            // The returned job handle owns provider-native polling, cancellation,
            // final log draining, and cleanup. Logging is delivered one line at a
            // time; it is intentionally not presented as a live byte stream.
            cloud::write_command_record(std::cout, "status", "submitting");
            const cloud::job submitted = client.run(spec);
            cloud::write_command_record(std::cout, "job_id", submitted.id());
            const cloud::result result = submitted.wait([](const cloud::log_entry& line) {
                cloud::write_command_record(std::cout, "log", line.text);
            });
            cloud::command_output::job_result(result).write(std::cout);
            if (!result.success()) {
                cloud::write_command_record(std::cerr, "error", result.error());
                cloud::write_command_record(std::cout, "status", "failed");
                return 1;
            }

            // get_file() downloads to a temporary file, verifies library CRC32C
            // metadata when present, and then replaces the destination. The calling
            // code remains identical for every route.
            client.storage().get_file("cloud://sim-output/result.txt", "./result.txt");
            cloud::write_command_record(std::cout, "status", "succeeded");
            return 0;
        } catch (...) {
            cloud::write_command_record(std::cout, "status", "failed");
            throw;
        }
    } catch (const std::exception& failure) {
        cloud::write_command_record(std::cerr, "error", failure.what());
        return 2;
    }
}
