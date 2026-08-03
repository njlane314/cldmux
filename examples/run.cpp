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

        // The library supplies the standard command records. This executable
        // adds its own record before choosing where to write the result.
        cloud::command_output output =
            cloud::command_output::diagnostics(chosen.provider, job, report);
        output.add("program", "cloud-run");
        output.write(std::cout);
        if (!chosen.submit) {
            cloud::write_command_record(std::cout, "status", "dry-run");
            return 0;
        }

        try {
            cloud::write_command_record(std::cout, "status", "submitting");
            std::cout.flush();
            const cloud::job submitted = routed.run(job);
            cloud::write_command_record(std::cout, "job_id", submitted.id());
            const cloud::result result = submitted.wait(
                [](const cloud::log_entry& line) {
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
        cloud::write_command_record(std::cerr, "error", failure.what());
        return 2;
    }
}
