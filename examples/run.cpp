#include "support.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>

int main(int argc, char* argv[]) {
    try {
        const cloud_example::options chosen = cloud_example::parse_options(
            argc, argv, "cheapest", std::chrono::minutes(5), "cloud-run");

        // "cheapest" compares every configured provider. Supplying gcp, aws,
        // or azure is the explicit override; the job itself does not change.
        cloud::client router = cloud_example::make_client(chosen);

        // Member assignment keeps this portable to C++17. C++20 and newer
        // callers may still use designated initialisers for these aggregates.
        cloud::job_spec job;
        job.name = "hello-cloud";
        job.image = "ubuntu:24.04";
        job.command = {"/bin/echo", "hello"};
        job.resources.cpus = 4;
        job.resources.memory_gb = 16;
        job.resources.spot = false;
        job.resources.max_price_per_hour = std::nullopt;
        job.retries = 1;
        job.auto_delete = true;
        job.timeout = std::chrono::minutes(15);

        // diagnose() plans exactly once without allocating resources. Binding
        // that reported provider makes the later submission match the visible
        // routing decision; run() still revalidates and refreshes its price.
        const cloud::run_diagnostics report =
            router.diagnose(job, chosen.expected_attempt_runtime);
        cloud::client routed = router.route(report.selected_plan.provider);
        cloud_example::print_diagnostics(chosen, job, report);
        cloud_example::print_record(std::cout, "preflight", "planned");
        if (!chosen.submit) {
            cloud_example::print_record(std::cout, "status", "dry-run");
            return 0;
        }

        try {
            cloud_example::print_record(std::cout, "status", "submitting");
            std::cout.flush();
            const cloud::job submitted = routed.run(job);
            cloud_example::print_record(std::cout, "job_id", submitted.id());
            const cloud::result result = submitted.wait(
                [](const cloud::log_entry& line) {
                    cloud_example::print_record(std::cout, "log", line.text);
                });
            cloud_example::print_result(result);
            if (!result.success()) {
                cloud_example::print_record(std::cerr, "error", result.error());
                cloud_example::print_record(std::cout, "status", "failed");
                return 1;
            }
            cloud_example::print_record(std::cout, "status", "succeeded");
            return 0;
        } catch (...) {
            cloud_example::print_record(std::cout, "status", "failed");
            throw;
        }
    } catch (const std::exception& failure) {
        cloud_example::print_record(std::cerr, "error", failure.what());
        return 2;
    }
}
