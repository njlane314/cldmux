# CLDMUX

[![Build](https://github.com/njlane314/cldmux/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/njlane314/cldmux/actions/workflows/ci.yml)
![C++17 to C++23](https://img.shields.io/badge/C%2B%2B-17%20to%2023-00599C)

`cldmux` is a header-only C++17 library for running provider-neutral container
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
#include <cldmux>

#include <chrono>
#include <iostream>

int main() {
    auto router = cldmux::router::from_environment("cheapest");
    cldmux::job_spec job;
    job.name = "hello-cldmux";
    job.image = "ubuntu:24.04";
    job.command = {"/bin/echo", "hello"};
    job.resources.cpus = 4;
    job.resources.memory_gb = 16;
    job.retries = 1;
    job.timeout = std::chrono::minutes(15);

    const auto report = router.diagnose(job, std::chrono::minutes(5));
    auto output = cldmux::command_output::diagnostics("cheapest", job, report);
    output.add("program", "example");
    std::cout << output;
}
```

Build with:

```sh
c++ -std=c++17 -I. example.cpp -lcurl -pthread -o cldmux-run
```

The repository has two heavily commented examples. [`example.cpp`](example.cpp)
is a provider-neutral command and an author checklist covering routing, pricing,
resources, mounts, storage, raw compute, diagnostics, output, and lifecycle
choices. It diagnoses and compares by default; only `--submit` permits billable
work. [`example.mk`](example.mk) constructs a file-based DAG whose processing
nodes can run in the same worker image through local Docker or GCP Batch:

```sh
make -f example.mk help
make -f example.mk dag
make -f example.mk -j2 cloud-plan       # offline provider-shape planning
make -f example.mk -j2 local IMAGE=REGISTRY/WORKER@sha256:DIGEST
# After exporting the GCP and dispatch variables shown by `help`:
make -f example.mk -j2 cloud APPROVE_CLOUD=YES PIPELINE_ID=RUN-VERSION \
    IMAGE=REGISTRY/WORKER@sha256:DIGEST
```

The Make example deliberately names the one workload component the repository
does not provide: a digest-pinned image containing `/app/pipeline-worker`. Its
comments define that worker contract, local and cloud commands, approval gates,
immutable run IDs, receipts, recovery boundaries, and parallel fan-out/fan-in.
Host-side bundle construction uses `tar` and `zstd`; local worker execution also
requires Docker or a compatible container command.

## ROUTING

Choose a policy at run time without changing the job:

```sh
cldmux-run                                  # choose the cheapest provider
cldmux-run gcp                              # explicit override
cldmux-run aws --estimate                   # price one explicit provider
cldmux-run azure --expected-attempt-runtime=20m --estimate --submit
```

`router::from_environment("cheapest")` requires at least two configured
providers. It plans and prices the same job on every configured provider, then
selects the lowest comparable USD compute estimate in stable GCP, AWS, Azure
order. A failed plan or missing quote aborts the comparison; the router never
silently chooses from a partial set.

Passing `gcp`, `aws`, or `azure` overrides the router. Public-catalogue lookup
then remains disabled unless the application explicitly selects it:

```cpp
auto router = cldmux::router::from_environment(
    "aws", cldmux::price_source::public_catalogue);
```

Provider-owned operations require a bound client:

```cpp
auto routed = router.route(job);       // Plan once and bind the winner.
auto aws = router.route("aws");        // Bind an explicit override.

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

- GCP requires `CLDMUX_GCP_PROJECT`; region and zone may be supplied through
  `CLDMUX_GCP_REGION` and `CLDMUX_GCP_ZONE`.

- AWS on-demand jobs require `CLDMUX_AWS_JOB_QUEUE` and
  `CLDMUX_AWS_MACHINE_TYPE`; Spot adds `CLDMUX_AWS_SPOT_JOB_QUEUE`,
  `CLDMUX_AWS_SPOT_MACHINE_TYPE`, and `CLDMUX_AWS_ZONE`. Mounted jobs instead
  require `CLDMUX_AWS_FARGATE_JOB_QUEUE` (or
  `CLDMUX_AWS_FARGATE_SPOT_JOB_QUEUE`), role variables, and a
  `CLDMUX_AWS_S3_FILES_*` mapping. Two mounts use the corresponding
  `CLDMUX_AWS_S3_FILES_INPUT_*` and `CLDMUX_AWS_S3_FILES_OUTPUT_*` mappings.
  GPU queues are configured separately.

- Azure requires `CLDMUX_AZURE_BATCH_ENDPOINT`. Its submission token is read
  from `CLDMUX_AZURE_BATCH_TOKEN` only if Azure is selected and `run()` is
  called.

`CLDMUX_REGION` and `CLDMUX_ZONE` are shared fallbacks. Explicit `cldmux::config`
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

`cldmux::command_output` renders stable, ordered `KEY=value` records and lets an
application amend them before writing:

```cpp
auto output = cldmux::command_output::diagnostics("cheapest", job, report);
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
policy. Copies of one `cldmux::job` share controller state; serialise calls to
`status()`, `logs()`, `wait()`, and `cancel()`.

## PRICING

Public-catalogue lookup is opt-in because planning then performs network
requests:

```cpp
config.prices = cldmux::price_source::public_catalogue;
```

The built-in lookup uses GCP Cloud Billing, the signed AWS Price List or EC2
Spot APIs, and Azure Retail Prices. USD compute list prices are cached for one
hour, or five minutes for Spot. Disks, storage, network, licences, taxes,
discounts, and free tiers are excluded.

AWS needs an exact machine type for EC2 on-demand pricing and an Availability
Zone for Spot history. Mounted on-demand jobs compose Fargate vCPU and rounded
memory prices; Fargate Spot requires a trusted callback. GCP A2 Ultra and A3
High built-in estimates are unavailable because inseparable Local SSD charges
prevent an honest compute-only value.

Applications may override lookup with a current internal price source:

```cpp
config.lookup_hourly_cost = [](const cldmux::price_request& request)
        -> std::optional<double> {
    return lookup_in_your_current_price_table(request);
};
```

Callbacks return a finite, non-negative USD hourly rate or `std::nullopt`.
`resources::max_price_per_hour` fails closed when the estimate is absent or too
high. An earlier plan is never a reservation; `run()` prices again.

## DISPATCH APPLICATION

[`apps/dispatch.hpp`](apps/dispatch.hpp) is a provider-neutral, two-phase adapter.
`prepare()` validates, quotes, and pins without allocating compute. Passing its
move-only result to `execute()` is the caller's approval. Link
`apps/dispatch.cpp` with `DISPATCH_NO_MAIN` when embedding that adapter without
the bundled command-line entry point.

```sh
make dispatch
export DISPATCH_INPUT_ROOT=cloud://dispatch-input
export DISPATCH_OUTPUT_ROOT=cloud://dispatch-output
build/check/cldmux-dispatch --id=simulation-0042 --image=IMAGE@sha256:DIGEST \
    --input=case.tar.zst --output=result.tar.zst -- /app/solve
# Review the KEY=value quote, then repeat with --submit to approve it.
```

The dry run reports the route, hourly price, expected runtime, and advisory cost.
`--policy=gcp`, `aws`, or `azure` overrides cheapest routing; an unpriced
submission also needs `--allow-unpriced`.

The container reads `/dispatch/input/runs/ID/input.tar.zst`, writes
`/dispatch/output/runs/ID/output.tar.zst`, emits line-oriented progress, and exits
zero on success. The two roots must name different buckets/containers so input
stays read-only. AWS needs both named S3 Files mappings and is CPU-only here.

`execute()` uses a create-only input upload and no-clobber local publication. Its
grep-friendly receipt records the image digest, SHA-256 hashes, quote, route,
elapsed time, result, and recovery state. An interruption leaves `.pending` and
.pending.EXECUTION_ID`; do not resubmit blindly. Dispatch is not a scheduler,
credential store, provisioner, archiver, or workflow engine.

[`example.mk`](example.mk) shows how Make can supply the missing workflow layer:
Make owns the DAG and concurrency, a worker image owns one transformation, and
dispatch owns each complete cloud artifact transaction. The host controller
downloads every cloud node before releasing its dependants.

## DISPATCH BINARIES

The library remains the single `cldmux` header. These targets build the dispatch
command into fixed platform-specific paths:

```sh
make dispatch-native   # current host only
make dispatch-macos    # build/bin/macos/cldmux-dispatch
make dispatch-linux    # build/bin/linux/cldmux-dispatch
make dispatch-windows  # build/bin/windows/cldmux-dispatch.exe
make dispatch-binaries # all three targets
```

A native target uses `CXX`, `curl-config`, and the current platform's libcurl.
A foreign target requires its target toolchain and libcurl settings through
`MACOS_*`, `LINUX_*`, or `WINDOWS_*`: set `*_CXX`, optional `*_TARGET_FLAGS`,
target-specific `*_CURL_CXXFLAGS` and `*_CURL_LIBS`, and `*_STRIP` when the
compiler cannot locate a target-aware strip tool. Foreign targets leave the curl
settings empty instead of accidentally linking the host library. Each target
also checks the compiler triple and predefined OS macro before compiling.
`dispatch-binaries` therefore needs all three target toolchains, target libcurl
installations, and a macOS SDK. Each fixed output path holds one configured
architecture/ABI at a time and is overwritten when rebuilt for another one.
Platform binaries use hidden visibility and are stripped after linking. Ordinary
tests, examples, and sanitizer executables remain diagnosable but live beneath
the mode-`0700` `build/check` directory rather than a shared temporary directory.
If an older checkout created `build` with broader permissions, remove it or use
a new relative `BUILD_DIR`; the Makefile will not silently change its mode.
The fixed platform targets are development artifacts; `release-macos` is the
credentialed distribution path.

### macOS release

`make check-release-macos` exercises the production byte order without private
credentials: compile thin arm64 and x86-64 slices, merge, strip, sign the final
universal bytes ad hoc, and strictly verify every architecture. That output is a
release-structure test, not a distributable signature.

For distribution, store notarization credentials in Keychain and pass only
public identity/profile names to the fail-closed release target:

```sh
xcrun notarytool store-credentials cldmux-notary \
    --key /secure/AuthKey_KEYID.p8 --key-id KEYID --issuer ISSUER_UUID
RELEASE_VERSION=0.5.0 \
MACOS_SIGN_IDENTITY='Developer ID Application: NAME (TEAMID)' \
MACOS_INSTALLER_IDENTITY='Developer ID Installer: NAME (TEAMID)' \
MACOS_NOTARY_PROFILE=cldmux-notary \
    make release-macos
```

The release version must match `CLDMUX_VERSION`, the source tree must be clean,
and production bytes are built from the recorded commit snapshot. The default
deployment floor is macOS 13.0 (`MACOS_MIN_VERSION` can raise it). The target
refuses missing or ad-hoc identities. It Developer-ID-signs the
final universal executable with the hardened runtime and secure timestamp,
builds a signed flat installer, waits for Apple notarization, staples and
validates the ticket, runs Gatekeeper assessment, and writes the package, signed
binary, permission-preserving binary tarball, MIT licence, source commit,
notarization records, and SHA-256 files under
`build/release/macos/RELEASE_VERSION/`. Do not pass Apple passwords or private
keys as Make variables.

Before enabling the manual `macOS Release` workflow, create a `release`
environment in GitHub, restrict it explicitly to `main`, add a required reviewer,
and store every `APPLE_*` value as an environment secret. The repository does not
currently create or protect that environment for you. Before approval, the
reviewer should verify that CI and CodeQL passed for the workflow's exact commit.
The workflow validates the branch and source version before entering the
environment, uses an ephemeral Keychain, then deletes the imported keys before
upload. The environment requires a base64-encoded
combined Developer ID PKCS#12 and App Store Connect team key, their passwords and
IDs, plus the two certificate identity names. It uploads the accepted installer,
notarization records, and a tarball that preserves the signed binary's executable
mode, but does not publish a GitHub Release automatically.
Signing and notarization credentials remain in Keychain and are never compiled
into either release artifact.

The Windows rule targets MinGW, not `cl.exe`. Run it entirely inside one MSYS2
UCRT64 or MINGW64 environment so the compiler and curl metadata have the same
ABI; explicit metadata is also accepted:

```sh
make dispatch-windows WINDOWS_CXX=x86_64-w64-mingw32-g++ \
    WINDOWS_CURL_CXXFLAGS="$(pkg-config --cflags libcurl)" \
    WINDOWS_CURL_LIBS="$(pkg-config --libs libcurl)"
```

The outputs are executables rather than self-contained installers: deploy the
dynamic libcurl, TLS, and compiler runtime libraries they use. On Windows,
place dispatch artefacts and receipts in ACL-protected directories on a
hard-link-capable filesystem; their no-clobber publication relies on hard
links. Local Windows paths must be representable by the active code page and
fit the normal Windows path-length limit.

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
VM size. `router::from_environment()` loads one mapping through
`CLDMUX_COMPUTE_TEMPLATE` and the corresponding provider variables; explicit
`cldmux::config` supports larger maps. Google-specific primitives remain under
`cldmux::gcp`.

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
router.supports("gcp", cldmux::feature::spot_instances); // true
router.supports("aws", cldmux::feature::containers);     // true
client.supports(cldmux::feature::accelerators);           // bound provider
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

`cldmux` is the only public header. Place it on the compiler's include path and
include it like a standard-library header:

```cpp
#include <cldmux>
```

```text
apps/dispatch.hpp            provider-neutral dispatch API
apps/dispatch.cpp            dispatch implementation and portable command
cldmux                       generated public header
example.cpp                  commented library/command example
example.mk                   commented local/GCP Make DAG example
include/cldmux/               private generator fragments
include/cldmux/detail/        private transport, pricing, and submission fragments
include/cldmux/detail/providers/
tools/amalgamate.cpp         standalone C++17 generator
tests/compile/               first-include and ODR probes
```

Never edit the generated `cldmux` header. Change the private fragments and run:

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

`make check` compiles every private fragment, tests the generator, verifies the
generated public header, probes multi-translation-unit use, and builds the
example and dispatch applications. Tests use loopback fakes and
offline quotes; they need no cloud credentials and cannot incur charges. The
Make example is expanded for both backends without starting Docker or approving
a cloud submission. The separate least-privilege CodeQL workflow manually builds
the C++17 surface with the extended security query suite; macOS CI also verifies
the universal merge/strip/final-sign order, and Linux CI verifies a stripped
distribution binary. Native MinGW UCRT64 CI compiles and runs the dispatch core,
exercises Windows-native path/no-clobber behaviour, and verifies that the PE
command is stripped without an MSYS runtime dependency.

## LICENCE

[MIT](LICENSE)
