# cloud

`cloud.hpp` is one C++20 header for running a container on temporary cloud
compute:

```text
local data → plan → upload → run → poll logs → collect output → delete
```

The job-facing model is provider-independent. The v0.1 implementation and its
low-level types and endpoints are deliberately GCP-specific: Batch for jobs,
Cloud Storage for data, Cloud Logging for output, and Compute Engine as an
escape hatch. AWS, Azure, GPUs, and built-in price lookup are not implemented;
requests for them fail clearly.

It uses the REST APIs directly through libcurl. There is no SDK, daemon,
generated code, Python or Go helper, or implementation `.cpp`; it never shells
out to provider CLIs.

## Example

```cpp
#include "cloud.hpp"

#include <iostream>

int main() {
  cloud::config config;
  config.project = "physics-project";
  config.region = "europe";  // maps to europe-west4
  config.zone = "europe-west4-a";  // only needed for raw compute()
  config.auth = cloud::auth::default_chain();

  cloud::client client(std::move(config));

  client.storage().put_file(
      "cloud://sim-input/run-42/config.json", "./config.json");

  cloud::job_spec spec{
      .name = "simulation-42",
      .image = "ghcr.io/example/simulation:latest",
      .command = {"/usr/local/bin/simulate",
                  "--config", "/input/config.json",
                  "--output", "/output/result.json"},
      .workdir = {},
      .service_account =
          "batch-runner@physics-project.iam.gserviceaccount.com",
      .mounts = {
          {"cloud://sim-input/run-42/", "/input", true},
          {"cloud://sim-output/run-42/", "/output"},
      },
      .resources = {
          .cpus = 4,
          .memory_gb = 16,
          .gpu = {},
          .spot = true,
          .max_price_per_hour = std::nullopt,
      },
      .retries = 2,
      .auto_delete = true,
      .timeout = std::chrono::hours(2),
  };

  const auto plan = client.plan(spec);
  std::cout << plan.provider << ' '
            << plan.region << ' ' << plan.machine_type << '\n';

  auto job = client.run(spec);
  const auto result = job.wait([](const cloud::log_entry& line) {
    std::cout << line.text << '\n';
  });

  if (!result.success()) {
    std::cerr << result.error() << '\n';
    return 1;
  }

  client.storage().get_file(
      "cloud://sim-output/run-42/result.json", "./result.json");
}
```

Build with:

```sh
c++ -std=c++20 main.cpp -lcurl -pthread
```

The header is fully inline; do not define an implementation macro.

## What `run()` does

`plan()` validates the requested capabilities and maps CPU/RAM requirements to
the smallest built-in `e2-standard-*` shape. `run()` makes a fresh plan, then
submits one direct-argv container task to GCP Batch. Batch provisions the VM,
applies the retry and per-attempt runtime policy, runs the container, and
removes its temporary compute. `job::wait()` polls job state and Cloud Logging.
At the controller deadline it starts cancellation; final log draining and
cleanup can make the call return later. With `auto_delete`, it also deletes the
Batch job record.

Commands are not passed through a shell. `command[0]` becomes the container
entrypoint and the remaining elements stay separate arguments.

Mount sources denote a GCS bucket or prefix, not one object. Batch mounts them
with GCS FUSE and bind-mounts the resulting directory at the requested container
path. A read-only mount must set the third field to `true`. `workdir` sets the
container working directory; it does not upload local source code. Put code in
the image or in an explicitly mounted prefix. Buckets and prefixes are not
created by `run()`.

Cloud Logging is polled and can be delayed; this is log streaming at the line
level, not a live byte stream. The quiet period and maximum final drain are
configurable through `final_log_delay` and `final_log_timeout`. Because Cloud
Logging is eventually visible, even the final drain is best effort.

A `job` and all of its copies share one mutable controller state. Serialize
calls to `status()`, `logs()`, `wait()`, and `cancel()`; concurrent calls on the
same job are unsupported.

## Cost policy

`cloud.hpp` never fabricates prices. Supply a small trusted callback if a plan
must contain an hourly estimate:

```cpp
config.estimate_hourly_cost =
    [](std::string_view provider, std::string_view region,
       std::string_view machine, bool spot) -> std::optional<double> {
  return lookup_in_your_current_price_table(provider, region, machine, spot);
};
```

Then `resources::max_price_per_hour` is enforced before submission. A maximum
without an estimator fails closed. Estimates are advisory, never guarantees;
`run()` plans again, so an earlier returned plan is not a reservation or binding
quote. Egress is currently reported as unknown. `selection::lowest_cost` also
fails unless there is an honest set of implemented providers to compare.

## Storage and raw compute

The narrow storage surface is:

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

## Authentication and safety

The default chain checks fixed bearer-token environment variables, an
authorized-user Application Default Credentials file, then the GCE metadata
service. An explicit token callback can supply federation, impersonation, or a
different credential system. Service-account key JSON and external-account ADC
JSON are not parsed by this small header.

The library never invokes `gcloud`, `aws`, or `az`, and never logs credentials.
API endpoints require TLS unless insecure HTTP is explicitly enabled for a
test. Batch and GCE mutation requests carry idempotency UUIDs, destructive
operations target one named resource, project SSH keys are blocked on Batch VMs,
and no firewall rules or inbound ports are created. Batch VMs can still receive
external IP addresses unless the project supplies a private network/NAT policy.

Enable the Batch, Compute Engine, Cloud Logging, and Cloud Storage APIs first.
The referenced buckets, instance templates, and custom service accounts must
already exist. The controller identity needs permission to create/delete Batch
jobs and act as the job service account, plus Logs Viewer and any object
permissions used through `storage()`. The Batch VM service account needs Batch
Agent Reporter, Logs Writer, and least-privilege read/write access to its mounted
GCS prefixes; private images may also require registry access.

`job_spec::timeout` is enforced by the waiting controller. GCP Batch's
`maxRunDuration` applies to each attempt, so retries can outlive that duration
if the controller disappears. Batch still owns and cleans up its temporary VMs.

## Capabilities and limits

```cpp
client.supports("gcp", cloud::feature::spot_instances); // true
client.supports("aws", cloud::feature::containers);     // false
client.supports(cloud::feature::accelerators);           // false
```

The built-in machine table covers up to 32 vCPU and 128 GiB. Generic `europe`,
`us`, and `asia` aliases are convenience defaults, not latency or carbon
optimizers. Provider-specific regions can be supplied directly.

## Test

```sh
make check
make sanitize
```

Tests are C++ only and use an in-process loopback fake server. They make no cloud
API calls, need no credentials, and cannot incur cloud charges.

MIT licensed.
