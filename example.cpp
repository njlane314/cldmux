#include "cloud.h"

#include <chrono>
#include <iostream>
#include <string_view>
#include <utility>

int main(int argc, char* argv[]) {
    // This example is safe to run without arguments: it builds and prints a
    // provider-native plan, but it does not submit a job. Pass --submit only
    // after replacing every placeholder below with resources from your cloud
    // account. A submitted job can consume billable compute and storage.
    const bool submit = argc == 2 && std::string_view{argv[1]} == "--submit";

    // This walkthrough uses GCP so its concrete service account, mounts, and
    // storage calls are internally consistent. The job model also supports AWS
    // and Azure; the README shows their required queue and Batch-endpoint setup.
    cloud::config config;
    config.provider = "gcp";
    config.project = "physics-project";
    config.region = "europe";       // GCP alias for europe-west4.
    config.zone = "europe-west4-a"; // Needed by raw GCE, not this Batch plan.

    // The default GCP chain checks an explicit environment bearer token,
    // authorised-user ADC, and finally the GCE metadata service. See the README
    // for the corresponding AWS credential and Azure OAuth configuration.
    config.auth = cloud::auth::default_chain();

    // Public price lookup is deliberately opt-in because plan() then performs
    // network requests. Leave it disabled for a purely local, deterministic
    // plan, or enable the following line for a current list-price estimate:
    // config.prices = cloud::price_source::public_catalog;

    // Short polling intervals are useful in tests, but real cloud control
    // planes should use the defaults. These settings bound only the final log
    // drain; config.cleanup_timeout separately bounds deletion and recovery.
    config.final_log_delay = std::chrono::seconds(2);
    config.final_log_timeout = std::chrono::seconds(30);

    // Moving the configuration makes one client own its callbacks and cached
    // price data. The client itself performs no submission here.
    cloud::client client(std::move(config));

    // A job_spec describes the portable part of one container invocation.
    // Commands are passed as tokens, never through a shell. For GCP the first
    // token becomes the entrypoint; AWS and Azure preserve the image ENTRYPOINT
    // and pass these tokens as its command arguments.
    // Ordinary member assignment makes the example valid in C++17 as well as
    // every newer standard. The structures remain aggregates, so applications
    // compiled as C++20 or later may use designated initialisers if preferred.
    cloud::job_spec spec;
    spec.name = "simulation-42";
    spec.image = "ghcr.io/example/simulation:latest";
    spec.command = {"/usr/local/bin/simulate", "--config", "/input/config.json", "--output",
                    "/output/result.json"};

    // GCP can run the container as a dedicated service account. The controller
    // identity must be allowed to act as this account.
    spec.service_account = "batch-runner@physics-project.iam.gserviceaccount.com";

    // cloud:// mounts currently map to GCS prefixes. The trailing slash is
    // intentional: a mount names a directory-like prefix, not one object.
    // The final boolean marks the input mount read-only.
    spec.mounts = {
        {"cloud://sim-input/run-42/", "/input", true},
        {"cloud://sim-output/run-42/", "/output"},
    };

    // The planner chooses the smallest supported native shape. Set gpu to t4,
    // l4, a10, a100, or h100 to request an accelerator. AWS GPU jobs
    // additionally need a dedicated queue mapping in config.aws.gpu_targets.
    spec.resources.cpus = 4;
    spec.resources.memory_gb = 16;
    spec.resources.gpu_count = 1;
    spec.resources.spot = true;

    // A ceiling fails closed if no trustworthy estimate exists. It is omitted
    // here because public lookup is disabled above.
    spec.resources.max_price_per_hour = std::nullopt;

    // Retries are delegated to the provider's Batch service. auto_delete
    // removes the GCP/Azure job record after the terminal logs are drained; AWS
    // retains its terminal record but deregisters the temporary job definition.
    spec.retries = 2;
    spec.auto_delete = true;
    spec.timeout = std::chrono::hours(2);

    // Planning validates the entire request and resolves a concrete region,
    // machine type, accelerator, and optional hourly estimate. It never
    // allocates compute; public pricing only adds read-only catalogue requests.
    const cloud::plan plan = client.plan(spec);
    std::cout << "provider: " << plan.provider << '\n'
              << "region:   " << plan.region << '\n'
              << "machine:  " << plan.machine_type << '\n';

    if (!submit) {
        std::cout << "dry run only; pass --submit after configuring real resources\n";
        return 0;
    }

    // Everything below this point can make authenticated, billable cloud API
    // calls. Uploading through storage() is GCP-only in this release; AWS and
    // Azure users should stage inputs with their normal object-storage tooling.
    client.storage().put_file("cloud://sim-input/run-42/config.json", "./config.json");

    // run() plans once more immediately before submission so validation and
    // price ceilings are fresh. The returned handle owns provider-specific
    // polling, cancellation, final log draining, and cleanup behaviour.
    cloud::job job = client.run(spec);
    const cloud::result result = job.wait([](const cloud::log_entry& line) {
        // Cloud logging services provide line-level polling, not a byte stream.
        std::cout << line.text << '\n';
    });

    if (!result.success()) {
        std::cerr << "job failed: " << result.error() << '\n';
        return 1;
    }

    // Download through a temporary file and verified CRC32C before replacing
    // the destination. As with the upload, this facade currently selects GCS.
    client.storage().get_file("cloud://sim-output/run-42/result.json", "./result.json");
    return 0;
}
