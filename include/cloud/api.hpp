#pragma once

#include "cloud/detail/providers/gcp.hpp"

// Public API: portable jobs, storage, and raw compute -------------------------

namespace cloud {

// Storage and instance values deliberately use one compact representation for
// every provider. Native identifiers remain provider-shaped strings.
using error = gcp::Error;
using access_token = gcp::AccessToken;
using token_provider = std::function<access_token()>;
using scoped_token_provider = std::function<access_token(std::string_view)>;
using object = gcp::Object;
using object_list = gcp::ObjectList;
using list_options = gcp::ListOptions;
using put_options = gcp::PutOptions;
using instance = gcp::Instance;

using provider = std::string;

// Provider-neutral asynchronous raw-compute operation. GCE long-running
// operations and AWS/Azure state transitions expose the same bounded wait()
// surface without pretending their native operation resources are identical.
class operation {
public:
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& location() const noexcept { return location_; }
    // Compatibility spelling retained for callers of the original GCE handle.
    [[nodiscard]] const std::string& zone() const noexcept { return location_; }

    void wait(std::chrono::milliseconds timeout = std::chrono::minutes(10),
              std::chrono::milliseconds poll_interval = std::chrono::seconds(1)) const {
        if (timeout <= std::chrono::milliseconds::zero())
            throw error("Compute operation wait timeout must be positive");
        if (poll_interval <= std::chrono::milliseconds::zero())
            throw error("Compute operation poll interval must be positive");
        waiter_(timeout, poll_interval);
    }

private:
    friend class compute;
    using waiter = std::function<void(std::chrono::milliseconds, std::chrono::milliseconds)>;

    operation(std::string name, std::string location, waiter wait)
        : name_(std::move(name)), location_(std::move(location)), waiter_(std::move(wait)) {
        if (name_.empty() || !waiter_)
            throw error("Malformed compute operation");
    }

    std::string name_;
    std::string location_;
    waiter waiter_;
};

// ordered selects the first configured provider that can plan the request.
// lowest_cost requires comparable hourly prices and at least two candidates.
enum class selection { ordered, lowest_cost };

// Catalogue pricing is opt-in because it adds read-only network requests during
// plan(). Caller callbacks below take precedence over this setting.
enum class price_source { none, public_catalogue };

// Capability flags describe implemented library behaviour only. They do not
// imply credentials, quota, regional availability, or configured AWS queues.
enum class feature {
    object_storage,
    containers,
    spot_instances,
    storage_mounts,
    log_streaming,
    raw_instances,
    accelerators,
    cost_estimates,
};

// Lazy bearer authentication for GCP and Azure. AWS uses aws_config because it
// signs each request rather than sending a bearer token. A scoped callback is
// invoked with the GCP OAuth scope or Azure Batch audience and cached per scope.
class auth {
public:
    auth() : credentials_(gcp::Credentials::automatic()) {}
    static auth default_chain() { return {}; }
    static auth bearer(std::string token) {
        return auth(gcp::Credentials::bearer(std::move(token)));
    }
    static auth from(token_provider callback) {
        return auth(gcp::Credentials::from(std::move(callback)));
    }
    static auth from(scoped_token_provider callback) {
        if (!callback)
            throw error("Scoped token provider must not be empty");
        return auth(std::make_shared<scoped_token_provider>(std::move(callback)));
    }

private:
    friend struct detail::client_state;
    explicit auth(gcp::Credentials credentials) : credentials_(std::move(credentials)) {}
    explicit auth(std::shared_ptr<scoped_token_provider> callback)
        : credentials_(gcp::Credentials::from([callback] {
              return (*callback)("https://www.googleapis.com/auth/cloud-platform");
          })),
          scoped_(std::move(callback)) {}

    [[nodiscard]] gcp::Credentials for_scope(std::string scope) const {
        if (!scoped_)
            return credentials_;
        const auto callback = scoped_;
        return gcp::Credentials::from(
            [callback, scope = std::move(scope)] { return (*callback)(scope); });
    }
    gcp::Credentials credentials_;
    std::shared_ptr<scoped_token_provider> scoped_;
};

// Portable minimum resources. Planning rounds CPU/RAM up to a supported native
// shape; it never silently substitutes a different accelerator model or count.
struct resources {
    unsigned cpus = 1;
    double memory_gb = 1;
    // Canonical values are "t4", "l4", "a10", "a100", and "h100".
    // The selected backend resolves the model to a native machine/accelerator.
    std::string gpu;
    unsigned gpu_count = 1;
    bool spot = false;
    std::optional<double> max_price_per_hour;
};

// A provider-neutral native storage mount. source is cloud://bucket or a
// provider-supported slash-terminated prefix; target is an absolute path in
// the container. Planning rejects combinations the selected backend cannot
// represent faithfully.
struct mount {
    std::string source;
    std::string target;
    bool read_only = false;
};

struct job_spec {
    std::string name = "job";
    std::string image;

    // Shell-free tokens. GCP uses element zero as the container entrypoint. AWS
    // and Azure preserve the image ENTRYPOINT and pass every token as arguments;
    // Azure therefore accepts only its documented portable token characters.
    std::vector<std::string> command;
    std::string workdir;

    // GCP interprets this as the VM service-account email, AWS as jobRoleArn,
    // and Azure rejects it because its auto-pool has no equivalent job field.
    std::string service_account;
    std::vector<mount> mounts;
    cloud::resources resources;

    // retries requests this many policy retries after the first attempt.
    // Qualifying failures differ by provider, and provider-managed recovery or
    // requeueing may occur outside this budget.
    unsigned retries = 0;

    // GCP deletes the Batch job; AWS keeps the terminal record but deregisters
    // its temporary definition. Azure always terminates the job to release its
    // job-lifetime pool and deletes the record only when auto_delete is true.
    bool auto_delete = true;

    // Waiting-controller deadline, measured from creation of the submitted job
    // handle. It is also mapped to GCP/AWS per-attempt caps and Azure task
    // wall-clock time. It is not a provider-wide wall-clock upper bound: retries,
    // queueing, cancellation, cleanup, or a vanished controller can extend the
    // provider resource lifetime.
    std::chrono::milliseconds timeout{std::chrono::hours(1)};
};

struct plan {
    // Native location/shape selected without allocating compute. Estimates are
    // advisory; a maximum price fails closed unless an estimate is available.
    cloud::provider provider = "gcp";
    std::string region;
    std::string machine_type;
    std::string accelerator;
    unsigned accelerator_count = 0;
    std::optional<double> estimated_hourly_cost;
    // Reserved for a future comparable egress model; currently always empty.
    std::optional<double> estimated_egress_cost;
    std::vector<std::string> warnings;
};

// Read-only, pre-submission facts for one planned run. Durations expose the
// controller contract separately from the provider payload so callers do not
// need provider-specific timeout or retry arithmetic. Costs use the selected
// plan's advisory USD hourly estimate; public-catalogue estimates are
// compute-only, while callback contents remain caller-defined. Unavailable
// prices remain unavailable rather than being reported as zero.
struct run_diagnostics {
    cloud::plan selected_plan;
    // Caller-modelled active duration for one attempt. Queueing, provisioning,
    // retries, recovery, cancellation, and cleanup are outside this value.
    std::chrono::milliseconds expected_attempt_runtime{};
    std::chrono::milliseconds controller_timeout{};
    std::chrono::seconds provider_attempt_timeout{};
    // Azure also receives a job-level watchdog measured from Azure job creation
    // and covering the controller timeout plus cleanup, final-log, and request
    // allowances. Other providers leave it empty because their submitted job
    // has no equivalent total watchdog.
    std::optional<std::chrono::seconds> provider_job_timeout;
    unsigned configured_retries = 0;
    // This is the caller-configured retry-policy budget, not a bound on internal
    // provider recovery, requeueing, or Spot-preemption behaviour.
    unsigned configured_attempt_limit = 1;
    // Selected hourly estimate multiplied only by expected_attempt_runtime.
    // This is a sensitivity calculation, not an expected bill or
    // billable-lifetime model.
    std::optional<double> estimated_cost_for_expected_attempt_runtime;
    std::vector<std::string> warnings;
};

// Provider lifecycle states are normalised to this small common state machine.
// unknown means the response was valid but did not map to a recognised state.
enum class job_state {
    queued,
    scheduled,
    running,
    succeeded,
    failed,
    cancelling,
    cancelled,
    deleting,
    unknown,
};

struct log_entry {
    // timestamp is provider-native: RFC3339 on GCP, epoch-millisecond text on
    // AWS, and a synthetic sortable generation/stream/offset key on Azure.
    std::string timestamp;
    // receive_timestamp and id are best-effort provider metadata and may be
    // empty. text is one complete line; polling is not a byte stream.
    std::string receive_timestamp;
    std::string id;
    std::string text;
    std::string severity;
};

struct result {
    job_state state = job_state::unknown;
    // Exit status is best effort because some terminal provider records expose
    // no task/container status by the time the controller inspects them.
    std::optional<int> exit_code;
    std::string message;
    std::vector<std::string> warnings;
    [[nodiscard]] bool success() const noexcept { return state == job_state::succeeded; }
    [[nodiscard]] const std::string& error() const noexcept { return message; }
};

// One shell-variable-shaped KEY=value record. command_output preserves record
// order and repeated keys (notably warning=), escapes control characters, and
// never invokes a shell. Applications can amend the standard records before
// writing them to stdout, stderr, a file, or any other std::ostream.
struct command_record {
    std::string key;
    std::string value;
};

class command_output {
public:
    // Preserve both the caller's requested route (for example, cheapest) and
    // the concrete provider selected in report. spec must be the job passed to
    // diagnose(); the formatter intentionally performs no second validation.
    // The output remains a mutable value, so producing it has no hidden I/O.
    [[nodiscard]] static command_output diagnostics(std::string_view requested_provider,
                                                    const job_spec& spec,
                                                    const run_diagnostics& report) {
        command_output out;
        out.add("output_version", "1");
        out.add("requested_provider", requested_provider);
        out.add("job_name", spec.name);
        out.add("provider", report.selected_plan.provider);
        out.add("region", report.selected_plan.region);
        out.add("machine", report.selected_plan.machine_type);
        out.add_unsigned("requested_cpus", spec.resources.cpus);
        out.add_number("requested_memory_gb", spec.resources.memory_gb);
        out.add("accelerator", report.selected_plan.accelerator.empty()
                                   ? std::string("none")
                                   : report.selected_plan.accelerator);
        out.add_unsigned("accelerator_count", report.selected_plan.accelerator_count);
        out.add_boolean("spot", spec.resources.spot);
        out.add_duration_seconds("expected_attempt_runtime_seconds",
                                 report.expected_attempt_runtime);
        out.add_duration_seconds("controller_timeout_seconds", report.controller_timeout);
        out.add_integer("provider_attempt_timeout_seconds",
                        report.provider_attempt_timeout.count());
        if (report.provider_job_timeout)
            out.add_integer("provider_job_timeout_seconds",
                            report.provider_job_timeout->count());
        else
            out.add("provider_job_timeout_seconds", "not-applicable");
        out.add_unsigned("configured_retries", report.configured_retries);
        out.add_unsigned("configured_attempt_limit", report.configured_attempt_limit);
        out.add("cost_currency", "USD");
        if (report.selected_plan.estimated_hourly_cost)
            out.add_number("hourly_rate_estimate_usd",
                           *report.selected_plan.estimated_hourly_cost);
        else
            out.add("hourly_rate_estimate_usd", "unavailable");
        if (report.estimated_cost_for_expected_attempt_runtime)
            out.add_number("estimated_cost_for_expected_attempt_runtime_usd",
                           *report.estimated_cost_for_expected_attempt_runtime);
        else
            out.add("estimated_cost_for_expected_attempt_runtime_usd", "unavailable");
        out.add("estimate_basis", "expected-attempt-runtime-times-hourly-rate");
        for (const auto& warning : report.warnings)
            out.add("warning", warning);
        out.add("preflight", "planned");
        return out;
    }

    // result.message is deliberately absent: the application decides whether
    // an error belongs on stdout, stderr, or somewhere else.
    [[nodiscard]] static command_output job_result(const cloud::result& value) {
        command_output out;
        out.add("job_state", state_name(value.state));
        if (value.exit_code)
            out.add_integer("exit_code", *value.exit_code);
        for (const auto& warning : value.warnings)
            out.add("warning", warning);
        return out;
    }

    // add() preserves duplicates. set() replaces the first matching record and
    // removes later duplicates, appending a new record when the key is absent.
    command_output& add(std::string_view key, std::string_view value) {
        validate_key(key);
        command_record record{std::string(key), std::string(value)};
        records_.push_back(std::move(record));
        return *this;
    }

    command_output& add_number(std::string_view key, double value) {
        return add(key, number(value));
    }

    command_output& add_integer(std::string_view key, std::int64_t value) {
        return add(key, std::to_string(value));
    }

    command_output& add_unsigned(std::string_view key, std::uint64_t value) {
        return add(key, std::to_string(value));
    }

    command_output& add_boolean(std::string_view key, bool value) {
        return add(key, value ? "true" : "false");
    }

    command_output& add_duration_seconds(std::string_view key,
                                         std::chrono::milliseconds value) {
        return add_number(key, std::chrono::duration<double>(value).count());
    }

    command_output& set(std::string_view key, std::string_view value) {
        validate_key(key);
        const std::string key_copy(key);
        const std::string replacement(value);
        const auto first = std::find_if(records_.begin(), records_.end(), [&](const auto& item) {
            return item.key == key_copy;
        });
        if (first == records_.end())
            return add(key_copy, replacement);
        first->value = replacement;
        records_.erase(std::remove_if(std::next(first), records_.end(), [&](const auto& item) {
                           return item.key == key_copy;
                       }),
                       records_.end());
        return *this;
    }

    command_output& erase(std::string_view key) {
        const std::string key_copy(key);
        records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const auto& item) {
                           return item.key == key_copy;
                       }),
                       records_.end());
        return *this;
    }

    command_output& rename(std::string_view key, std::string_view replacement) {
        validate_key(replacement);
        const std::string key_copy(key);
        const std::string replacement_copy(replacement);
        for (auto& item : records_)
            if (item.key == key_copy)
                item.key = replacement_copy;
        return *this;
    }

    // Expose inspection without allowing callers to bypass key validation.
    [[nodiscard]] const std::vector<command_record>& records() const noexcept {
        return records_;
    }

    // Values stay unescaped in records_ and are escaped exactly once here.
    std::ostream& write(std::ostream& output) const {
        static constexpr char hex[] = "0123456789abcdef";
        for (const auto& item : records_) {
            std::string line = item.key;
            line.push_back('=');
            for (const char raw : item.value) {
                const auto c = static_cast<unsigned char>(raw);
                if (c == '\\')
                    line += "\\\\";
                else if (c == '\n')
                    line += "\\n";
                else if (c == '\r')
                    line += "\\r";
                else if (c == '\t')
                    line += "\\t";
                else if (c < 0x20 || c == 0x7f) {
                    line += "\\x";
                    line.push_back(hex[c >> 4]);
                    line.push_back(hex[c & 0x0f]);
                } else
                    line.push_back(raw);
            }
            line.push_back('\n');
            output.write(line.data(), static_cast<std::streamsize>(line.size()));
        }
        return output;
    }

    friend std::ostream& operator<<(std::ostream& output, const command_output& value) {
        return value.write(output);
    }

private:
    static void validate_key(std::string_view key) {
        const auto letter = [](char value) {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        };
        if (key.empty() || (!letter(key.front()) && key.front() != '_'))
            throw error("Command output keys must be shell variable names");
        for (const char value : key)
            if (!letter(value) && (value < '0' || value > '9') && value != '_')
                throw error("Command output keys must be shell variable names");
    }

    static std::string number(double value) {
        if (!std::isfinite(value))
            throw error("Command output numbers must be finite");
        std::ostringstream formatted;
        formatted.imbue(std::locale::classic());
        formatted << std::setprecision(12) << value;
        return formatted.str();
    }

    static std::string state_name(job_state state) {
        switch (state) {
        case job_state::queued:
            return "queued";
        case job_state::scheduled:
            return "scheduled";
        case job_state::running:
            return "running";
        case job_state::succeeded:
            return "succeeded";
        case job_state::failed:
            return "failed";
        case job_state::cancelling:
            return "cancelling";
        case job_state::cancelled:
            return "cancelled";
        case job_state::deleting:
            return "deleting";
        case job_state::unknown:
            return "unknown";
        }
        return "unknown";
    }

    std::vector<command_record> records_;
};

// Live lifecycle and log records should be written as they occur rather than
// accumulated in a command_output. This one-record path applies exactly the
// same validation and escaping while leaving flushing to the application.
inline std::ostream& write_command_record(std::ostream& output, std::string_view key,
                                          std::string_view value) {
    command_output record;
    record.add(key, value);
    return record.write(output);
}

struct price_request {
    // Exact native plan sent to a trusted caller callback. Returning nullopt
    // means no quote is available; otherwise the result must be finite,
    // nonnegative USD per hour. max_price_per_hour then fails closed.
    cloud::provider provider;
    std::string region;
    std::string zone;
    std::string machine_type;
    std::string accelerator;
    unsigned accelerator_count = 0;
    bool spot = false;
    // Needed for resource-priced services such as Fargate, where no EC2-style
    // machine identifier encodes the billed CPU and memory quantities. These
    // fields are appended to preserve older positional aggregate initialisers.
    unsigned cpus = 0;
    double memory_gb = 0;
};
using price_lookup = std::function<std::optional<double>(const price_request&)>;
// Compatibility callback with the same USD-per-hour result contract. New code
// should use lookup_hourly_cost so attached accelerators and an Availability
// Zone cannot be accidentally omitted.
using price_estimator = std::function<std::optional<double>(std::string_view, std::string_view,
                                                            std::string_view, bool)>;
using log_sink = std::function<void(const log_entry&)>;

struct aws_credentials {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
};
using aws_credential_provider = std::function<aws_credentials()>;

struct aws_gpu_target {
    // AWS Batch cannot choose a GPU model per job. Each entry declares queues
    // constrained to one instance type and the capacity that planner may trust.
    std::string job_queue;
    std::string spot_job_queue;
    std::string machine_type;
    unsigned cpus = 0;
    double memory_gb = 0;
    unsigned gpus = 0;
};

// One S3 Files file system already associated with a bucket. AWS Batch mounts
// the file system, while cloud:// continues to name the corresponding bucket.
struct aws_s3_files_mount {
    std::string file_system_arn;
    std::string access_point_arn;
};

// A logical raw-instance template can contain one native definition per cloud.
// Callers pass only the logical map key to compute().create().
struct aws_launch_template {
    std::string id;
    std::string name;
    std::string version = "$Default";
};

struct azure_instance_template {
    // A specialised managed image or Compute Gallery image resource ID avoids
    // embedding provider-specific login/password policy in the common API.
    std::string image_id;
    std::string subnet_id;
    std::string machine_type;
    std::string location;
    std::string os_disk_type = "Standard_LRS";
};

struct instance_template {
    std::string gcp_instance_template;
    cloud::aws_launch_template aws;
    cloud::azure_instance_template azure;
};

struct aws_config {
    // The refresh callback wins, followed by explicit values, then
    // AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, and optional AWS_SESSION_TOKEN.
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
    aws_credential_provider credentials;
    // AWS Batch queues own capacity and networking. A separate Spot queue is
    // required when resources::spot is true.
    std::string job_queue;
    std::string spot_job_queue;
    // S3 Files is supported by AWS Batch only on Fargate. Mounted jobs use
    // these queues and roles instead of the EC2 queues above.
    std::string fargate_job_queue;
    std::string fargate_spot_job_queue;
    std::string execution_role_arn;
    std::string job_role_arn;
    std::map<std::string, aws_s3_files_mount, std::less<>> s3_files;
    // Batch cannot select an EC2 GPU model per job. Map a canonical GPU name to
    // queues backed only by one exact instance type and declare that type's
    // capacity; planning fails rather than overcommitting it.
    std::map<std::string, aws_gpu_target, std::less<>> gpu_targets;
    // Optional exact types for dedicated CPU queues. Without these, Batch still
    // runs jobs but no exact EC2 catalogue price is claimed.
    std::string machine_type;
    std::string spot_machine_type;
    std::string log_group = "/aws/batch/job";
    // Empty regional endpoints are derived from the selected AWS region.
    std::string batch_endpoint;
    std::string logs_endpoint;
    std::string ec2_endpoint;
    std::string s3_endpoint;
    std::string pricing_endpoint = "https://api.pricing.us-east-1.amazonaws.com";
    std::string pricing_region = "us-east-1";
};

struct azure_config {
    // For example: https://account.westeurope.batch.azure.com
    std::string batch_endpoint;
    // When absent, config::auth supplies the Batch bearer token. This override
    // lets a multi-provider client use an Azure-scoped token independently.
    std::optional<cloud::auth> auth;
    // Blob and Resource Manager use different OAuth audiences from Batch.
    // Separate overrides preserve correct token caching in multi-cloud clients.
    std::string storage_account;
    std::optional<cloud::auth> storage_auth;
    // A SAS is used only by Batch nodes for BlobFuse mounts. Controller object
    // operations continue to prefer the storage-scoped bearer token above.
    std::string storage_sas;
    std::string storage_endpoint;
    std::string subscription_id;
    std::string resource_group;
    std::optional<cloud::auth> management_auth;
    std::string management_endpoint = "https://management.azure.com";
    std::string compute_api_version = "2025-04-01";
    // Image fields describe the auto-pool node image, not the submitted
    // container. Each job receives a one-node job-lifetime pool.
    std::string image_publisher = "microsoft-dsvm";
    std::string image_offer = "ubuntu-hpc";
    std::string image_sku = "2204";
    std::string node_agent_sku = "batch.node.ubuntu 22.04";
    std::string api_version = "2025-06-01";
    std::string pricing_endpoint = "https://prices.azure.com/api/retail/prices";
};

struct config {
    // provider forces exactly one backend. Otherwise providers is considered in
    // order. ordered returns the first runnable plan; lowest_cost requires priced
    // comparable candidates and at least two runnable providers.
    std::optional<cloud::provider> provider;
    std::vector<cloud::provider> providers{"gcp"};
    cloud::selection selection = cloud::selection::ordered;

    // project is GCP-specific. Per-provider maps override the shared region/zone
    // fallbacks, allowing one multi-provider client to carry native locations.
    std::string project;
    std::string region = "europe";
    std::string zone;
    std::map<cloud::provider, std::string, std::less<>> regions;
    std::map<cloud::provider, std::string, std::less<>> zones;

    // GCP and Azure bearer authentication; AWS credentials live in aws_config.
    cloud::auth auth = cloud::auth::default_chain();
    cloud::aws_config aws;
    cloud::azure_config azure;
    // Many logical templates may be supplied in C++. from_environment() keeps
    // its contract concise by loading at most one named CLOUD_COMPUTE_TEMPLATE.
    std::map<std::string, cloud::instance_template, std::less<>> instance_templates;

    // Price precedence is lookup_hourly_cost, compatibility estimator, then the
    // opt-in public catalogue. Supplying either callback suppresses catalogue lookup.
    cloud::price_source prices = cloud::price_source::none;
    price_lookup lookup_hourly_cost;
    price_estimator estimate_hourly_cost;
    std::chrono::milliseconds price_cache_ttl{std::chrono::hours(1)};
    std::chrono::milliseconds spot_price_cache_ttl{std::chrono::minutes(5)};

    // request_timeout bounds ordinary control-plane HTTP calls; transfer_timeout
    // applies to files. poll_interval controls job/LRO polling. Final log drain
    // waits at least final_log_delay for quiet but never beyond final_log_timeout.
    // cleanup_timeout bounds provider cleanup and ambiguous-mutation recovery.
    std::chrono::milliseconds request_timeout{std::chrono::minutes(1)};
    std::chrono::milliseconds transfer_timeout{std::chrono::hours(1)};
    std::chrono::milliseconds poll_interval{std::chrono::seconds(2)};
    std::chrono::milliseconds final_log_delay{std::chrono::seconds(2)};
    std::chrono::milliseconds final_log_timeout{std::chrono::seconds(30)};
    std::chrono::milliseconds cleanup_timeout{std::chrono::minutes(5)};

    // Endpoint overrides support emulators, tests, and private proxies. They are
    // GCP endpoints; AWS/Azure endpoints live in their nested configurations.
    // Plain HTTP always requires this explicit test-only escape hatch.
    bool allow_insecure_http = false;
    std::string storage_endpoint = "https://storage.googleapis.com";
    std::string compute_endpoint = "https://compute.googleapis.com";
    std::string batch_endpoint = "https://batch.googleapis.com";
    std::string logging_endpoint = "https://logging.googleapis.com";
    std::string billing_endpoint = "https://cloudbilling.googleapis.com";
};

} // namespace cloud
