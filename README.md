# CLOUD

[![Build](https://github.com/njlane314/cloud/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/njlane314/cloud/actions/workflows/ci.yml)
![C++17 to C++23](https://img.shields.io/badge/C%2B%2B-17%20to%2023-00599C)

`cloud` is a header-only C++17 library for running provider-neutral container
jobs on GCP, AWS, or Azure. It can compare public-catalogue compute prices,
route a job to the cheapest configured provider, or honour an explicit provider
override.

The library talks directly to provider REST APIs through libcurl. It needs no
cloud SDK, daemon, provider CLI, or user-authored JSON. Public configuration is
typed C++ or grep-friendly `KEY=value` environment variables.

```text
local data → plan → upload → run → poll logs → collect output → delete
```

## EXAMPLE

```cpp
#include "cloud.h"

#include <chrono>
#include <iostream>

int main() {
    auto client = cloud::client::from_environment("cheapest");
    cloud::job_spec job;
    job.name = "hello-cloud";
    job.image = "ubuntu:24.04";
    job.command = {"/bin/echo", "hello"};
    job.resources.cpus = 4;
    job.resources.memory_gb = 16;
    job.retries = 1;
    job.timeout = std::chrono::minutes(15);

    const auto report = client.diagnose(job, std::chrono::minutes(5));
    auto output = cloud::command_output::diagnostics("cheapest", job, report);
    output.add("program", "example");
    std::cout << output;
}
```

Build with:

```sh
c++ -std=c++17 -Iinclude -I. examples/run.cpp -lcurl -pthread -o cloud-run
```

[`examples/run.cpp`](examples/run.cpp) is a minimal provider-neutral command.
It diagnoses and compares by default; only `--submit` permits billable work.
The heavily commented [`example.cpp`](example.cpp) explains the wider API.

## ROUTING

Choose a policy at run time without changing the job:

```sh
cloud-run                                  # choose the cheapest provider
cloud-run gcp                              # explicit override
cloud-run aws --estimate                   # price one explicit provider
cloud-run azure --expected-attempt-runtime=20m --estimate --submit
```

`client::from_environment("cheapest")` requires at least two configured
providers. It plans and prices the same job on every configured provider, then
selects the lowest comparable USD compute estimate in stable GCP, AWS, Azure
order. A failed plan or missing quote aborts the comparison; the router never
silently chooses from a partial set.

Passing `gcp`, `aws`, or `azure` overrides the router. Public-catalogue lookup
then remains disabled unless the application explicitly selects it:

```cpp
auto client = cloud::client::from_environment(
    "aws", cloud::price_source::public_catalogue);
```

Provider-owned operations require a bound client:

```cpp
auto routed = client.route(job);       // Plan once and bind the winner.
auto azure = client.route("azure");    // Bind an explicit override.

routed.storage().put("cloud://inputs/data.txt", "payload");
routed.compute().create("worker", "general-worker").wait();
auto submitted = routed.run(job);
```

`route()` shares state without mutating the router. Storage, raw compute, and
submission remain on the selected provider. `run()` revalidates and reprices
that route immediately before submission, but cannot switch it to another
cloud.

## CONFIGURATION

`from_environment()` recognises these provider foundations:

- GCP requires `CLOUD_GCP_PROJECT`; region and zone may be supplied through
  `CLOUD_GCP_REGION` and `CLOUD_GCP_ZONE`.

- AWS on-demand jobs require `CLOUD_AWS_JOB_QUEUE` and
  `CLOUD_AWS_MACHINE_TYPE`; Spot adds `CLOUD_AWS_SPOT_JOB_QUEUE`,
  `CLOUD_AWS_SPOT_MACHINE_TYPE`, and `CLOUD_AWS_ZONE`. Mounted jobs instead
  require `CLOUD_AWS_FARGATE_JOB_QUEUE`, role variables, and a
  `CLOUD_AWS_S3_FILES_*` mapping. GPU queues are configured separately.

- Azure requires `CLOUD_AZURE_BATCH_ENDPOINT`. Its submission token is read
  from `CLOUD_AZURE_BATCH_TOKEN` only if Azure is selected and `run()` is
  called.

`CLOUD_REGION` and `CLOUD_ZONE` are shared fallbacks. Explicit `cloud::config`
supports provider-specific regions, several GPU targets, logical compute
templates, credential callbacks, and larger storage-mount maps.

The public Azure endpoint form is
`https://ACCOUNT.REGION.batch.azure.com`. Private proxies and sovereign-cloud
endpoints must be visible in explicit C++ configuration.

## DIAGNOSTICS AND COMMAND OUTPUT

`diagnose()` is read-only. It plans once and combines the chosen plan with the
caller's expected active runtime for one attempt:

```cpp
const auto report = router.diagnose(job, std::chrono::minutes(20));
auto routed = router.route(report.selected_plan.provider);

if (report.estimated_cost_for_expected_attempt_runtime)
    std::cout << *report.estimated_cost_for_expected_attempt_runtime << '\n';

auto submitted = routed.run(job);      // The first mutating operation.
```

The report includes the selected plan, hourly estimate, runtime sensitivity,
controller and provider timeouts, retry limit, and ordered warnings. The cost
is `hourly rate × expected active runtime` for one attempt. It is not a quote,
reservation, expected bill, or wall-clock prediction. Queueing, provisioning,
retries, storage, disks, network, licences, taxes, and discounts may change the
actual cost. Missing prices remain unavailable, never zero.

`preflight=planned` confirms local and provider-shape validation only. It does
not probe credentials, remote queues, images, objects, mounts, quota, capacity,
API reachability, or submission.

`cloud::command_output` renders stable, ordered `KEY=value` records and lets an
application amend them before writing:

```cpp
auto output = cloud::command_output::diagnostics("cheapest", job, report);
output.set("job_name", "nightly-simulation")
      .add("application", "simulation")
      .rename("machine", "selected_machine");
std::cout << output;
```

Typical dry-run output is deliberately easy to inspect with UNIX tools:

```text
requested_provider=cheapest
provider=aws
region=eu-west-1
machine=m6i.xlarge
expected_attempt_runtime_seconds=300
hourly_rate_estimate_usd=0.192
estimated_cost_for_expected_attempt_runtime_usd=0.016
preflight=planned
status=dry-run
```

`add()` appends, `set()` replaces, `rename()` preserves record position, and
`erase()` removes matching records. `records()` exposes the final order and
`job_result()` formats terminal state. `write_command_record()` emits live
status or log records. Keys follow `[A-Za-z_][A-Za-z0-9_]*`; values escape
control bytes and backslashes. Consumers split on the first `=`. This is a
display protocol, not shell source: do not pass it to `eval`.

## JOB EXECUTION

`plan()` validates capabilities and resolves a native machine. `run()` submits
to GCP Batch temporary VMs, an existing AWS Batch queue, or an Azure one-node
job-lifetime auto-pool. `job::wait()` polls status and line-oriented native
logs. At the controller deadline it starts cancellation; final log draining
and cleanup can return later.

Commands never pass through a shell. GCP treats `command[0]` as the entrypoint;
AWS and Azure preserve the image entrypoint and supply the command as
arguments. Azure accepts only its documented portable token characters and
fails closed on unsafe arguments.

Mounts expose provider-native storage collections:

- GCP mounts a bucket or slash-terminated prefix through GCS FUSE.

- AWS maps a bucket to configured [S3 Files](https://docs.aws.amazon.com/batch/latest/userguide/s3files-volumes.html)
  on Fargate. EC2 and mounted GPU jobs fail closed.

- Azure mounts a complete Blob container through [BlobFuse](https://learn.microsoft.com/en-us/azure/batch/virtual-file-mount).
  Prefix-only mounts fail closed.

`workdir` changes the container directory; it does not upload source. Images,
buckets, containers, prefixes, and native mount resources must already exist.
Logs are delayed, best-effort provider records rather than a live byte stream.

With `auto_delete`, GCP and Azure remove completed Batch jobs. AWS deregisters
the temporary job definition but retains its terminal job record under AWS
policy. Copies of one `cloud::job` share controller state; serialise calls to
`status()`, `logs()`, `wait()`, and `cancel()`.

## PRICING

Public-catalogue lookup is opt-in because planning then performs network
requests:

```cpp
config.prices = cloud::price_source::public_catalogue;
```

The built-in lookup uses GCP Cloud Billing, the signed AWS Price List or EC2
Spot APIs, and Azure Retail Prices. USD compute list prices are cached for one
hour, or five minutes for Spot. Disks, storage, network, licences, taxes,
discounts, and free tiers are excluded.

AWS needs an exact machine type for on-demand pricing and an Availability Zone
for Spot history. Mounted AWS jobs use Fargate, so their price requires a
trusted callback. GCP A2 Ultra and A3 High built-in estimates are unavailable
because inseparable Local SSD charges prevent an honest compute-only value.

Applications may override lookup with a current internal price source:

```cpp
config.lookup_hourly_cost = [](const cloud::price_request& request)
        -> std::optional<double> {
    return lookup_in_your_current_price_table(request);
};
```

Callbacks return a finite, non-negative USD hourly rate or `std::nullopt`.
`resources::max_price_per_hour` fails closed when the estimate is absent or too
high. An earlier plan is never a reservation; `run()` prices again.

## STORAGE AND RAW COMPUTE

`cloud://bucket/key` maps to GCS, S3, or Azure Blob Storage through the routed
client:

```cpp
client.storage().put(uri, bytes);
client.storage().put_file(uri, path);
client.storage().get(uri);
client.storage().get_file(uri, path);
client.storage().list(uri);
client.storage().stat(uri);
client.storage().remove(uri);
```

Transfers never cross providers. In-memory responses are limited to 16 MiB;
use `get_file()` for larger objects. Uploads are single-request rather than
resumable.

Raw instances use the same provider-neutral client:

```cpp
for (const auto& vm : client.compute().instances())
    std::cout << vm.name << ' ' << vm.status << '\n';

client.compute().create("worker", "general-worker").wait();
client.compute().stop("worker").wait();
client.compute().start("worker").wait();
client.compute().destroy("worker").wait();
```

The second `create()` argument is a logical template name. Configuration maps
it to a GCE instance template, EC2 launch template, or Azure image, subnet, and
VM size. `from_environment()` loads one mapping through
`CLOUD_COMPUTE_TEMPLATE` and the corresponding provider variables; explicit
`cloud::config` supports larger maps. Google-specific primitives remain under
`cloud::gcp`.

## AUTHENTICATION AND SAFETY

- GCP accepts fixed bearer tokens, an authorised-user ADC file generated by
  gcloud, the GCE metadata service, or a typed token callback.

- AWS uses libcurl SigV4 with a refresh callback, explicit fields, or
  `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, and `AWS_SESSION_TOKEN`.

- Azure accepts distinct Batch, Blob Storage, and Resource Manager tokens or a
  scoped callback for those audiences.

The library never invokes `gcloud`, `aws`, or `az`, and never logs credentials.
Its private provider-response codec is not a public configuration format.
Endpoints require TLS unless insecure HTTP is explicitly enabled for a test.
Mutations use request IDs or read-after-ambiguity checks, destructive calls
target one named resource, and the library creates no firewall rules or inbound
ports.

Queues, pools, storage resources, images, networks, templates, and service
accounts must exist before use. Grant the controller and workload identities
only the provider permissions required by their operations. Raw instance
actions target one name or ID; because EC2 `Name` tags are not unique, retain
the returned instance ID when independent controllers may race.

## CAPABILITIES AND LIMITS

```cpp
client.supports("gcp", cloud::feature::spot_instances); // true
client.supports("aws", cloud::feature::containers);     // true
client.supports(cloud::feature::accelerators);          // true
```

`supports()` reports implemented library behaviour, not account permissions,
regional quota, configured queues, or current capacity. All three providers
implement container jobs, object storage, native mounts, raw instances, logs,
accelerators, Spot planning, and cost estimates. Unsupported resource or mount
combinations fail instead of being silently substituted.

Canonical GPU names are `t4`, `l4`, `a10`, `a100`, and `h100`; provider and
machine support varies. AWS requires an explicit dedicated-queue mapping.
Generic `europe`, `us`, and `asia` locations are fixed convenience aliases, not
latency or carbon optimisers. Provider-native regions may be supplied directly.

## SOURCE LAYOUT

Use the modular headers for development:

```cpp
#include <cloud/cloud.hpp>
```

Use the generated single header when vendoring one file:

```cpp
#include "cloud.h"
```

```text
include/cloud/               canonical modular headers
include/cloud/detail/        transport, pricing, storage, and submission
include/cloud/detail/providers/
single_include/cloud.h       generated release header
cloud.h                      identical compatibility copy
tools/amalgamate.cpp         standalone C++17 generator
tests/compile/               first-include and ODR probes
```

Never edit either generated `cloud.h`. Change the modular source and run:

```sh
make amalgamate
```

The generator is deterministic and expands only strict project includes before
system includes or module content. Missing files, cycles, ambiguous directives,
and paths outside the include root fail closed. `--check` compares bytes without
rewriting a stale release; `--stdout` and `--list-modules` support build tooling.

## C++ COMPATIBILITY AND TESTS

C++17 is the minimum. CI checks the same public interface with GCC and Clang in
C++17, C++20, and C++23 modes; later modes do not enable a different API.

```sh
make check
make check-standards
make sanitise
```

`make check` compiles every modular header, tests the generator, checks both
generated headers byte-for-byte, runs modular and single-header suites, probes
multi-translation-unit use, and builds both examples. Tests use a loopback fake
server, make no cloud API calls, need no credentials, and cannot incur charges.

## LICENCE

[MIT](LICENSE)
