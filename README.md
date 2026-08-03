# CLOUD

[![Build](https://github.com/njlane314/cloud/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/njlane314/cloud/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)

`cloud.h` is one C++20 header for running a container on temporary cloud
compute:

Version 0.2 intentionally renames the sole public header from `cloud.hpp` to
`cloud.h`; update both the include and `CLOUD_H_VERSION` macro spelling.

```text
local data → plan → upload → run → poll logs → collect output → delete
```

The job-facing model is provider-independent. GCP Batch, AWS Batch, and Azure
Batch run container jobs; their native logging APIs feed one `cloud::job`
handle. GPU planning and opt-in public-catalog price lookup are built in.
Cloud Storage and raw instance control remain deliberately GCP-only and report
that boundary through `supports()`.

It uses the REST APIs directly through libcurl. There is no SDK, daemon,
generated code, Python or Go helper, or implementation `.cpp`; it never shells
out to provider CLIs.

## EXAMPLE

```cpp
#include "cloud.h"

#include <iostream>

int main() {
    cloud::config config;
    config.project = "physics-project";
    config.region = "europe";       // maps to europe-west4
    config.zone = "europe-west4-a"; // only needed for raw compute()
    config.auth = cloud::auth::default_chain();

    cloud::client client(std::move(config));

    client.storage().put_file("cloud://sim-input/run-42/config.json", "./config.json");

    cloud::job_spec spec{
        .name = "simulation-42",
        .image = "ghcr.io/example/simulation:latest",
        .command = {"/usr/local/bin/simulate", "--config", "/input/config.json", "--output",
                    "/output/result.json"},
        .workdir = {},
        .service_account = "batch-runner@physics-project.iam.gserviceaccount.com",
        .mounts =
            {
                {"cloud://sim-input/run-42/", "/input", true},
                {"cloud://sim-output/run-42/", "/output"},
            },
        .resources =
            {
                .cpus = 4,
                .memory_gb = 16,
                .gpu = {},
                .gpu_count = 1,
                .spot = true,
                .max_price_per_hour = std::nullopt,
            },
        .retries = 2,
        .auto_delete = true,
        .timeout = std::chrono::hours(2),
    };

    const auto plan = client.plan(spec);
    std::cout << plan.provider << ' ' << plan.region << ' ' << plan.machine_type << '\n';

    auto job = client.run(spec);
    const auto result =
        job.wait([](const cloud::log_entry& line) { std::cout << line.text << '\n'; });

    if (!result.success()) {
        std::cerr << result.error() << '\n';
        return 1;
    }

    client.storage().get_file("cloud://sim-output/run-42/result.json", "./result.json");
}
```

Build with:

```sh
c++ -std=c++20 example.cpp -lcurl -pthread
```

The header is fully inline; do not define an implementation macro. The
[annotated example](example.cpp) expands this workflow and defaults to a safe
planning-only run; submission requires an explicit `--submit` argument.

Select AWS with an existing Batch queue. Spot is a queue property, so it needs
a distinct queue:

```cpp
cloud::config config;
config.provider = "aws";
config.region = "eu-west-1";
config.aws.job_queue = "arn:aws:batch:eu-west-1:123456789012:job-queue/cpu";
config.aws.spot_job_queue = "arn:aws:batch:eu-west-1:123456789012:job-queue/cpu-spot";
// Explicit fields, a refresh callback, or the standard AWS environment names.
config.aws.credentials = [] {
    return cloud::aws_credentials{access_key(), secret_key(), session_token()};
};
cloud::client client(std::move(config));
```

AWS Batch cannot choose a GPU model in a job request. A GPU model must map to a
queue whose compute environment contains only that model, plus the exact EC2
type used for planning and prices:

```cpp
config.aws.gpu_targets["l4"] = {
    .job_queue = "arn:aws:batch:...:job-queue/l4",
    .spot_job_queue = "arn:aws:batch:...:job-queue/l4-spot",
    .machine_type = "g6.xlarge",
    .cpus = 4,
    .memory_gb = 16,
    .gpus = 1,
};
```

Azure uses a one-node job-lifetime auto-pool:

```cpp
cloud::config config;
config.provider = "azure";
config.region = "westeurope";
config.azure.batch_endpoint = "https://myaccount.westeurope.batch.azure.com";
config.azure.auth = cloud::auth::from([](std::string_view scope) { return token_for(scope); });
cloud::client client(std::move(config));
```

The scoped callback receives
`https://batch.core.windows.net/.default` for Azure Batch and the Google
Cloud Platform scope for GCP. A fixed `auth::bearer(...)` is also accepted.

## WHAT `run()` DOES

`plan()` validates the requested capabilities and resolves a native machine.
GCP uses `e2-standard-*` CPU shapes and Batch-owned temporary VMs. AWS submits
to a pre-provisioned queue after registering a job-specific definition. Azure
creates a one-node, job-lifetime auto-pool. All three apply retry and runtime
policies, and `job::wait()` polls state and logs. At the controller deadline it
starts cancellation; final log draining and cleanup can make the call return
later.

With `auto_delete`, GCP and Azure delete the Batch job. AWS does not expose job
record deletion; it deregisters the temporary job definition, while AWS keeps
the terminal job record according to its retention policy. Azure always
terminates the job after its task finishes so the job-lifetime auto-pool can be
released; `auto_delete = false` retains that completed job record.

Commands are not passed through a shell. On GCP, `command[0]` becomes the
container entrypoint and the remaining elements stay separate arguments. AWS
and Azure preserve the image `ENTRYPOINT` and supply `command` as its command
arguments; use an image without a conflicting entrypoint when `command[0]`
must itself be the executable. Azure Batch has one non-shell command-line
field, so this release accepts only portable token characters in each Azure
argument; unsafe or empty arguments fail during planning.

Mount sources are supported only by GCP and denote a GCS bucket or prefix, not
one object. Batch mounts them with GCS FUSE and bind-mounts the resulting
directory at the requested container path. A read-only mount must set the third
field to `true`. `workdir` sets the container working directory; it does not
upload local source code. Put code in the image or in an explicitly mounted
prefix. Buckets and prefixes are not created by `run()`.

Cloud Logging, CloudWatch Logs, and Azure task files are polled and can be
delayed; this is log streaming at the line level, not a live byte stream. Azure
stdout and stderr are grouped by stream because task files contain no shared
event timestamps. The quiet period and maximum final drain are configurable
through `final_log_delay` and `final_log_timeout`. Even the final drain is best
effort.

A `job` and all of its copies share one mutable controller state. Serialize
calls to `status()`, `logs()`, `wait()`, and `cancel()`; concurrent calls on the
same job are unsupported.

## COST POLICY

Public-catalog lookup is opt-in because it performs network requests during
planning:

```cpp
config.prices = cloud::price_source::public_catalog;
```

It queries the GCP Cloud Billing catalog, the signed AWS Price List API (or EC2
Spot price history), and the unauthenticated Azure Retail Prices API. Prices
are USD compute list-price estimates and are cached for one hour, or five
minutes for Spot. `price_cache_ttl` and `spot_price_cache_ttl` are configurable.
Disks, storage, network/egress, taxes, software licences, negotiated discounts,
and free tiers are excluded.

GCP A2 Ultra and A3 High estimates are deliberately unavailable because those
machine types include billed Local SSD that cannot be omitted from an honest
whole-machine ceiling. Supply `lookup_hourly_cost` for a trusted complete quote.

AWS on-demand pricing requires an exact `aws.machine_type` or
`aws.spot_machine_type`; a heterogeneous Batch queue is reported as
`batch-managed` and is not assigned a made-up price. AWS Spot lookup also
requires `config.zone`, because Spot observations are Availability-Zone
specific. A provider response with no unique applicable price leaves the
estimate unavailable.

A trusted callback remains available as an override and test seam:

```cpp
config.lookup_hourly_cost = [](const cloud::price_request& request) -> std::optional<double> {
    return lookup_in_your_current_price_table(request);
};
```

`resources::max_price_per_hour` is enforced before submission. A maximum
without an available estimate fails closed. Estimates are advisory, never
guarantees; `run()` plans again, so an earlier returned plan is not a
reservation or binding quote. Egress is reported as unknown.

`selection::lowest_cost` queries every configured provider, requires at least
two runnable providers with estimates, compares the same USD compute-only
basis, and follows provider order for ties. A missing quote fails closed.

## GCP STORAGE AND RAW COMPUTE

These provider-independent facades currently select the GCP implementation;
an AWS- or Azure-selected client throws instead of routing data to the wrong
provider. The narrow storage surface is:

```cpp
client.storage().put(uri, bytes);
client.storage().put_file(uri, path);
client.storage().get(uri);
client.storage().get_file(uri, path);
client.storage().list(uri);
client.storage().stat(uri);
client.storage().remove(uri);
```

`get()` and `get_file()` generation-pin downloads and verify CRC32C. `get_file()`
writes a temporary file before replacing the destination; replacement is atomic
on POSIX filesystems, while the Windows compatibility fallback is recoverable
but not atomic. File transfers default to a one-hour timeout. Uploads are
single-request, not resumable, and can use generation preconditions through
`cloud::put_options`.

Raw GCE control remains available when Batch is not the right abstraction:

```cpp
for (const auto& vm : client.compute().instances())
    std::cout << vm.name << ' ' << vm.status << '\n';

client.compute().create("worker", "instance-template").wait();
client.compute().stop("worker").wait();
client.compute().start("worker").wait();
client.compute().destroy("worker").wait();
```

`compute::create()` specifically creates from an existing global instance
template. The Google-specific primitives are also exposed under `cloud::gcp`.

## AUTHENTICATION AND SAFETY

The GCP default chain checks fixed bearer-token environment variables, an
authorized-user Application Default Credentials file, then the GCE metadata
service. An explicit token callback can supply federation, impersonation, or a
different credential system. Service-account key JSON and external-account ADC
JSON are not parsed by this small header.

AWS requests use libcurl's SigV4 implementation. The `aws.credentials` refresh
callback wins, followed by explicit `aws_config` fields, then
`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, and optional `AWS_SESSION_TOKEN`.
Profiles, SSO, ECS credentials, and EC2 metadata are intentionally not
parsed—inject them through the callback if needed.

Azure Batch requires an OAuth bearer token for the Batch audience. Use
`azure.auth` with a fixed bearer token or a scoped callback. The library does
not invoke Azure CLI or implement a client-secret/managed-identity exchange;
the callback is the boundary for those flows.

The library never invokes `gcloud`, `aws`, or `az`, and never logs credentials.
API endpoints require TLS unless insecure HTTP is explicitly enabled for a
test. GCP mutations carry request IDs; AWS uses unique randomized names plus
read-after-ambiguity reconciliation; Azure uses fixed IDs plus inspection before
replay. Destructive operations target one named resource, and the library
creates no firewall rules or inbound ports. On GCP, project SSH keys are blocked
on Batch VMs, but those VMs can still receive external IP addresses unless the
project supplies a private network/NAT policy. AWS inherits networking from the
configured queue/compute environment; Azure uses the Batch auto-pool network
behavior configured for the account.

For GCP, enable the Batch, Compute Engine, Cloud Logging, and Cloud Storage APIs
first. The referenced buckets, instance templates, and custom service accounts
must already exist. The controller identity needs permission to create/delete
Batch jobs and act as the job service account, plus Logs Viewer and any object
permissions used through `storage()`. The Batch VM service account needs Batch
Agent Reporter, Logs Writer, and least-privilege read/write access to its
mounted GCS prefixes; private images may also require registry access.

For AWS, create queues and compute environments first; GPU queues must use a
GPU-optimized AMI and constrain capacity to the declared model. The controller
needs Batch registration/submission/inspection/cancellation permissions and
CloudWatch Logs read access. For Azure, grant Batch data-plane access to the
Batch account and ensure the configured VM size/image is available in the
region.

`job_spec::timeout` is enforced by the waiting controller. GCP Batch's
`maxRunDuration` applies to each attempt, so retries can outlive that duration
if the controller disappears. Batch still owns and cleans up its temporary VMs.

## CAPABILITIES AND LIMITS

```cpp
client.supports("gcp", cloud::feature::spot_instances); // true
client.supports("aws", cloud::feature::containers);     // true
client.supports(cloud::feature::accelerators);          // true
```

The capability matrix is:

| Feature | GCP | AWS | Azure |
|---|---:|---:|---:|
| Container jobs, Spot, logs, accelerators, estimates | yes | yes | yes |
| Object storage, storage mounts, raw instances | yes | no | no |

`supports()` reports implemented library behavior, not account permissions,
regional quota, queue configuration, or current SKU capacity.

CPU tables cover GCP E2 and Azure Dsv5 through 32 vCPU/128 GiB. Canonical GPU
names are `t4`, `l4`, `a10`, `a100`, and `h100`. GCP maps T4, L4, A100 80 GB,
and H100 shapes; Azure maps T4, A10, A100 80 GB, and H100; AWS accepts any of the
canonical names only when it has an explicit dedicated-queue mapping. Unsupported
model/count/resource combinations fail rather than substitute a different GPU.

Generic `europe`, `us`, and `asia` aliases map to a documented default region
for each provider; they are convenience defaults, not latency or carbon
optimizers. Provider-specific regions can be supplied directly. Multi-provider
selection can give each backend its own location through `config.regions` and,
for AWS Spot observations, `config.zones`.

## TEST

```sh
make example
make check
make sanitize
```

The test cases use the sibling [`tst`](https://github.com/njlane314/tst)
single-header library and an in-process loopback fake server. They make no cloud
API calls, need no credentials, and cannot incur cloud charges. Override
`TST_DIR` when the header is not checked out at `../tst`.

## LICENSE

[MIT](LICENSE)
