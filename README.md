# CLOUD

[![Build](https://github.com/njlane314/cloud/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/njlane314/cloud/actions/workflows/ci.yml)
![C++17 to C++26](https://img.shields.io/badge/C%2B%2B-17%20to%2026-00599C)

`cloud.h` is one C++17-or-newer header for running a container on temporary cloud
compute:

Version 0.2 intentionally renamed the sole public header from `cloud.hpp` to
`cloud.h`; update both the include and `CLOUD_H_VERSION` macro spelling.
Version 0.3 makes the second `compute().create()` argument a logical template
key; map an earlier direct GCE template value through
`config.instance_templates[key].gcp_instance_template`. It also adopts the
British `price_source::public_catalogue` spelling.
Version 0.3.1 makes example output line-oriented `KEY=value`, limits in-memory
responses to 16 MiB, and bounds the private provider-response parser.

```text
local data → plan → upload → run → poll logs → collect output → delete
```

The job-facing model is provider-independent. GCP Batch, AWS Batch, and Azure
Batch run container jobs; their native logging APIs feed one `cloud::job`
handle. GPU planning and opt-in public-catalogue price lookup are built in.
The same selected provider owns `cloud://` object storage, native storage
mounts, and logical-template raw instance control.

It uses the REST APIs directly through libcurl. There is no SDK, daemon,
generated code, Python or Go helper, or implementation `.cpp`; it never shells
out to provider CLIs.

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

    const auto selected = client.plan(job);
    std::cout << "provider=" << selected.provider << '\n'
              << "region=" << selected.region << '\n'
              << "machine=" << selected.machine_type << '\n';
}
```

Build with:

```sh
c++ -std=c++17 -I. examples/run.cpp -lcurl -pthread -o cloud-run
```

The header is fully inline; do not define an implementation macro. The
minimal [`examples/run.cpp`](examples/run.cpp) keeps one provider-neutral job
definition. It defaults to a safe, planning-only price comparison; only
`--submit` permits billable execution. The heavily commented
[`example.cpp`](example.cpp) explains the fuller provider-neutral workflow.

## AUTOMATIC ROUTING

The first argument chooses the policy without changing or recompiling the job:

```sh
cloud-run                  # compare configured providers and choose the cheapest
cloud-run cheapest         # the same explicit policy
cloud-run gcp              # override the router
cloud-run aws
cloud-run azure --submit   # submit only after the explicit flag
```

Planning output is one stable record per line:

```text
provider=aws
region=eu-west-1
machine=m6i.xlarge
hourly_usd=0.192
warning=estimates are advisory
status=dry-run
```

`client::from_environment("cheapest")` checks configured providers in stable
GCP, AWS, Azure order, enables public-catalogue prices, and selects the lowest
USD compute estimate. At least two providers must be configured. Every included
provider must validate the same job and return a comparable price; one failed
plan or missing quote aborts the decision instead of biasing it towards a
partial set. `run()` repeats the comparison immediately before submission.

Passing `gcp`, `aws`, or `azure` is the override. It loads only that provider
and leaves catalogue lookup disabled, so `plan()` remains local. For job
submission, provider infrastructure stays outside the program:

| Provider | Required environment | Optional environment |
|---|---|---|
| GCP | `CLOUD_GCP_PROJECT` | `CLOUD_GCP_REGION`, `CLOUD_GCP_ZONE` |
| AWS | at least one CPU, Spot, or complete GPU queue; mounted jobs instead require a Fargate queue, roles, and S3 Files mapping | `CLOUD_AWS_REGION`, `CLOUD_AWS_MACHINE_TYPE`, `CLOUD_AWS_SPOT_MACHINE_TYPE`, `CLOUD_AWS_ZONE`, `CLOUD_AWS_LOG_GROUP` |
| Azure | `CLOUD_AZURE_BATCH_ENDPOINT` | `CLOUD_AZURE_REGION`, `CLOUD_AZURE_BATCH_TOKEN` for submission |

`CLOUD_REGION` and `CLOUD_ZONE` are shared fallbacks. For cheapest AWS routing,
an on-demand CPU path needs `CLOUD_AWS_JOB_QUEUE` and
`CLOUD_AWS_MACHINE_TYPE`; a Spot CPU path needs `CLOUD_AWS_SPOT_JOB_QUEUE`,
`CLOUD_AWS_SPOT_MACHINE_TYPE`, and `CLOUD_AWS_ZONE`. Both need
`AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`. An exact AWS machine type is a
declaration that its queue is constrained to that type; otherwise the estimate
cannot be trusted. GCP uses its existing credential chain, while Azure retail
pricing is unauthenticated.

`CLOUD_AZURE_BATCH_ENDPOINT` accepts the public Batch form
`https://ACCOUNT.REGION.batch.azure.com` only. The Azure Batch token is read
lazily if Azure wins and the job is submitted. Configure a trusted private
proxy or sovereign-cloud endpoint through explicit `cloud::config`, where the
destination is visible in code.

A cheapest-policy client is deliberately unbound until it has a job or an
explicit override. Bind it before touching provider-owned resources:

```cpp
auto routed = client.route(job);       // Plan once and bind the winner.
auto aws = client.route("aws");         // Explicit local override.

std::cout << routed.selected_provider() << '\n';
routed.storage().put("cloud://inputs/data.txt", "payload");
routed.compute().create("worker", "general-worker").wait();
auto submitted = routed.run(job);
```

`route()` returns a cheap `cloud::client` sharing the original state; it does
not mutate the router. Its storage, raw compute, planning, and submission stay
on the bound provider. `run()` revalidates and reprices the job, but cannot
switch that routed client to a different cloud.

One exact AWS GPU target can be supplied without adding provider logic to the
program: set `CLOUD_AWS_GPU_MODEL`, `CLOUD_AWS_GPU_MACHINE_TYPE`,
`CLOUD_AWS_GPU_CPUS`, `CLOUD_AWS_GPU_MEMORY_GB`, `CLOUD_AWS_GPU_COUNT`, and at
least one of `CLOUD_AWS_GPU_JOB_QUEUE` or `CLOUD_AWS_GPU_SPOT_JOB_QUEUE`.
Multiple AWS GPU targets should use an explicit `cloud::config`.

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
auto& target = config.aws.gpu_targets["l4"];
target.job_queue = "arn:aws:batch:...:job-queue/l4";
target.spot_job_queue = "arn:aws:batch:...:job-queue/l4-spot";
target.machine_type = "g6.xlarge";
target.cpus = 4;
target.memory_gb = 16;
target.gpus = 1;
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

The scoped callback receives the applicable Google Cloud Platform scope or the
Azure Batch, Blob Storage, or Resource Manager scope. A fixed
`auth::bearer(...)` is also accepted.

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

Mount sources denote storage collections, not individual objects. GCP accepts a
GCS bucket or slash-terminated prefix through GCS FUSE. AWS maps the source
bucket to a configured [S3 Files file
system](https://docs.aws.amazon.com/batch/latest/userguide/s3files-volumes.html)
and uses an AWS Batch Fargate queue; S3 Files mounts on EC2 and mounted GPU jobs
fail closed. Azure mounts a complete Blob container through [BlobFuse virtual
mounts](https://learn.microsoft.com/en-us/azure/batch/virtual-file-mount); a
prefix below a container fails closed. A read-only mount must set the third
field to `true`.

`workdir` sets the container working directory; it does not upload local source
code. Put code in the image or in an explicitly mounted collection. Buckets,
containers, prefixes, and native mount resources are not created by `run()`.

Cloud Logging, CloudWatch Logs, and Azure task files are polled and can be
delayed; this is log streaming at the line level, not a live byte stream. Azure
stdout and stderr are grouped by stream because task files contain no shared
event timestamps. The quiet period and maximum final drain are configurable
through `final_log_delay` and `final_log_timeout`. Even the final drain is best
effort.

A `job` and all of its copies share one mutable controller state. Serialise
calls to `status()`, `logs()`, `wait()`, and `cancel()`; concurrent calls on the
same job are unsupported.

## COST POLICY

Public-catalogue lookup is opt-in because it performs network requests during
planning:

```cpp
config.prices = cloud::price_source::public_catalogue;
```

It queries GCP Cloud Billing pricing data, the signed AWS Price List API (or EC2
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

Mounted AWS jobs run on Fargate, whose vCPU and rounded memory quantities are
exposed to `lookup_hourly_cost`. The built-in EC2 catalogue lookup deliberately
does not invent a Fargate quote, so lowest-cost routing for a mounted job needs
that callback or an explicit provider override.

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

`selection::lowest_cost` validates and prices every configured provider,
requires at least two of them, compares the same USD compute-only basis, and
follows provider order for ties. Any failed plan or missing quote fails closed.

## STORAGE AND RAW COMPUTE

`cloud://bucket/key` maps to GCS, S3, or Azure Blob Storage according to the
routed client. The URI contains no provider name because `route(job)` or
`route(provider)` is the authority; an unbound multi-provider client fails
closed instead of guessing. The narrow storage surface is:

```cpp
client.storage().put(uri, bytes);
client.storage().put_file(uri, path);
client.storage().get(uri);
client.storage().get_file(uri, path);
client.storage().list(uri);
client.storage().stat(uri);
client.storage().remove(uri);
```

Transfers stay within the routed provider; the facade never copies an object to
a different cloud. `get_file()` writes a temporary file before replacing the
destination; replacement is atomic on POSIX filesystems, while the Windows
compatibility fallback is recoverable but not atomic. File transfers default to
a one-hour timeout. In-memory responses are limited to 16 MiB; use `get_file()`
for larger objects. Uploads are single-request, not resumable.

Raw instance control remains available when Batch is not the right abstraction:

```cpp
for (const auto& vm : client.compute().instances())
    std::cout << vm.name << ' ' << vm.status << '\n';

client.compute().create("worker", "general-worker").wait();
client.compute().stop("worker").wait();
client.compute().start("worker").wait();
client.compute().destroy("worker").wait();
```

The second argument to `compute().create()` is a logical template name. The
routed client resolves it to a GCE instance template, an EC2 launch template, or
a specialised Azure managed/Compute Gallery image plus subnet and VM size. Explicit
`cloud::config` can define the same logical name for several providers; native
identifiers never enter the provider-neutral call site. Google-specific
primitives remain exposed under `cloud::gcp`.

Application configuration is plain, grep-friendly `KEY=value`; callers never
need to author JSON. The only optional file parser in the automatic credential
chain reads an authorised-user ADC file generated by gcloud.
`from_environment()` loads at most one logical compute template and one AWS S3
Files mapping, while explicit `cloud::config` supports larger maps:

```text
CLOUD_COMPUTE_TEMPLATE=general-worker
CLOUD_GCP_INSTANCE_TEMPLATE=projects/example/global/instanceTemplates/worker

CLOUD_AWS_LAUNCH_TEMPLATE_ID=lt-0123456789abcdef0
CLOUD_AWS_S3_FILES_BUCKET=inputs
CLOUD_AWS_S3_FILES_FILE_SYSTEM_ARN=<file-system-arn>
CLOUD_AWS_S3_FILES_ACCESS_POINT_ARN=<access-point-arn>  # optional
CLOUD_AWS_FARGATE_JOB_QUEUE=arn:aws:batch:...:job-queue/storage
CLOUD_AWS_EXECUTION_ROLE_ARN=arn:aws:iam::...:role/execution
CLOUD_AWS_JOB_ROLE_ARN=arn:aws:iam::...:role/job

CLOUD_AZURE_STORAGE_ACCOUNT=examplestorage
CLOUD_AZURE_STORAGE_TOKEN=...
CLOUD_AZURE_STORAGE_SAS=...                              # Batch mounts only
CLOUD_AZURE_SUBSCRIPTION_ID=...
CLOUD_AZURE_RESOURCE_GROUP=workers
CLOUD_AZURE_VM_IMAGE_ID=/subscriptions/.../images/worker
CLOUD_AZURE_VM_SUBNET_ID=/subscriptions/.../subnets/workers
CLOUD_AZURE_VM_SIZE=Standard_D4s_v5
```

AWS may use `CLOUD_AWS_LAUNCH_TEMPLATE_NAME` instead of the ID. Azure raw VM
operations use `CLOUD_AZURE_MANAGEMENT_TOKEN`; Blob controller operations use
the storage token, while the SAS is passed only to Batch nodes for mounts.

## AUTHENTICATION AND SAFETY

The GCP default chain checks fixed bearer-token environment variables, an
optional authorised-user ADC file generated by gcloud, then the GCE metadata
service used by an attached service account. Callers do not author that file;
an explicit typed token callback can instead supply federation, impersonation,
or another credential system.

AWS requests use libcurl's SigV4 implementation. The `aws.credentials` refresh
callback wins, followed by explicit `aws_config` fields, then
`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, and optional `AWS_SESSION_TOKEN`.
Profiles, SSO, ECS credentials, and EC2 metadata are intentionally not
parsed—inject them through the callback if needed.

Azure Batch, Blob Storage, and Resource Manager require tokens for distinct
audiences. Use the corresponding authentication override or a scoped callback.
The library does not invoke Azure CLI or implement a
client-secret/managed-identity exchange; the callback is the boundary for those
flows.

The library never invokes `gcloud`, `aws`, or `az`, and never logs credentials.
Callers author typed C++ and `KEY=value` environment configuration only. The
private transport codec serialises the provider-required REST wire format; it
is not a public configuration format. The examples escape control bytes and
backslashes in their `KEY=value` output. Advanced callers can deliberately
inspect a provider's raw diagnostic body through `cloud::error::response()`.

API endpoints require TLS unless insecure HTTP is explicitly enabled for a
test. GCP mutations carry request IDs; AWS Batch uses unique randomised names
plus read-after-ambiguity reconciliation; Azure uses fixed IDs plus inspection
before replay. Destructive operations target one named resource, and the library
creates no firewall rules or inbound ports. On GCP, project SSH keys are blocked
on Batch VMs, but those VMs can still receive external IP addresses unless the
project supplies a private network/NAT policy. AWS inherits networking from the
configured queue/compute environment; Azure uses the Batch auto-pool network
behaviour configured for the account.

EC2 `Name` tags are not unique. Raw creation checks for an existing managed name
before `RunInstances`, and later name-based actions reject ambiguity. A
concurrent-create race remains possible across independent controllers; retain
the returned instance ID and use it for lifecycle calls when coordinating them.

For GCP, enable the Batch, Compute Engine, Cloud Logging, and Cloud Storage APIs
first. Referenced buckets, instance templates, and custom service accounts must
already exist. The controller identity needs permission to create/delete
Batch jobs and act as the job service account, plus Logs Viewer and any object
permissions used through `storage()`. The Batch VM service account needs Batch
Agent Reporter, Logs Writer, and least-privilege read/write access to its
mounted GCS prefixes; private images may also require registry access.

For AWS, create queues and compute environments first; GPU queues must use a
GPU-optimised AMI and constrain capacity to the declared model. S3 Files file
systems, VPC mount targets, access points, and Fargate queues must also exist
before they are referenced. The controller needs the applicable Batch, EC2,
S3, and CloudWatch permissions. For Azure, create referenced Blob containers,
networks, and images first, grant the applicable Batch, Blob, and Resource
Manager access, and ensure the configured VM size/image is available in the
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

`supports()` reports implemented library behaviour, not account permissions,
regional quota, queue configuration, or current SKU capacity.

All three providers expose container jobs, object storage, native storage
mounts, raw instances, logs, accelerators, Spot planning, and cost estimates.
Their native semantics remain visible: GCP mounts may select prefixes, AWS S3
Files mounts require Fargate and reject GPUs, and Azure mounts require a whole
Blob container. Unsupported combinations fail closed.

CPU tables cover GCP E2 and Azure Dsv5 through 32 vCPU/128 GiB. Canonical GPU
names are `t4`, `l4`, `a10`, `a100`, and `h100`. GCP maps T4, L4, A100 80 GB,
and H100 shapes; Azure maps T4, A10, A100 80 GB, and H100; AWS accepts any of the
canonical names only when it has an explicit dedicated-queue mapping. Unsupported
model/count/resource combinations fail rather than substitute a different GPU.

Generic `europe`, `us`, and `asia` aliases map to a documented default region
for each provider; they are convenience defaults, not latency or carbon
optimisers. Provider-specific regions can be supplied directly. Multi-provider
selection can give each backend its own location through `config.regions` and,
for AWS Spot observations, `config.zones`.

## C++ COMPATIBILITY

C++17 is the minimum language version. The same public interface is checked in
C++17, C++20, C++23, and C++26 modes; later modes do not enable a different API.
The C++26 check verifies source compatibility with the evolving compiler mode,
not that a compiler implements every C++26 library facility.

The local C++26 target tries `-std=c++26` and then the older `-std=c++2c`
spelling. It reports a skip when the selected compiler supports neither; choose
a particular spelling with, for example,
`make check-c++26 CXX26_STANDARD=c++2c`. CI uses Clang 18's `-std=c++2c` mode,
so the C++26 check is required there rather than skipped.

## TEST

```sh
make example
make examples
make check
make check-c++20
make check-c++23
make check-c++26
make check-standards
make sanitize
```

`make check` uses the C++17 baseline. `make check-standards` checks every
supported language mode with the selected compiler; its C++26 step follows the
graceful probing behaviour described above.

In C++20 and newer modes, the test cases use the sibling
[`tst`](https://github.com/njlane314/tst) single-header library. C++17 uses a
small test-only fallback because `tst` uses C++20's `std::source_location`.
Every mode uses the same cases and an in-process loopback fake server. They make
no cloud API calls, need no credentials, and cannot incur cloud charges.
Override `TST_DIR` when the header is not checked out at `../tst`.

## LICENCE

[MIT](LICENSE)
