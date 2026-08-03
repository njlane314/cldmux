#pragma once

#include "cldmux/api.hpp"

namespace cldmux {

// Planning and provider validation --------------------------------------------

namespace detail {

struct uri {
    std::string bucket;
    std::string key;
};

inline uri parse_uri(std::string_view value, std::string_view operation = {}) {
    // cloud:// is deliberately provider-neutral: the route-bound client maps it
    // to GCS, S3, or Azure Blob Storage. Object operations require a key; list
    // and mount operations may address the collection root.
    constexpr std::string_view prefix = "cloud://";
    if (!gcp::detail::starts_with(value, prefix))
        throw error("Cloud URI must start with cloud://");
    value.remove_prefix(prefix.size());
    const auto slash = value.find('/');
    uri out{std::string(value.substr(0, slash)),
            slash == std::string_view::npos ? std::string{} : std::string(value.substr(slash + 1))};
    if (out.bucket.empty())
        throw error("Cloud URI must contain a bucket");
    if (!operation.empty() && out.key.empty())
        throw error(std::string(operation) + "() requires an object path");
    return out;
}

inline bool implemented(std::string_view value) {
    return value == "gcp" || value == "aws" || value == "azure";
}

inline bool supports(std::string_view value, feature requested) {
    if (!implemented(value))
        return false;
    switch (requested) {
    case feature::object_storage:
    case feature::containers:
    case feature::spot_instances:
    case feature::storage_mounts:
    case feature::log_streaming:
    case feature::raw_instances:
    case feature::accelerators:
    case feature::cost_estimates:
        return true;
    }
    return false;
}

inline std::string configured_region(const config& cfg, std::string_view provider) {
    const auto it = cfg.regions.find(provider);
    return it == cfg.regions.end() ? cfg.region : it->second;
}

inline std::string configured_zone(const config& cfg, std::string_view provider) {
    const auto it = cfg.zones.find(provider);
    return it == cfg.zones.end() ? cfg.zone : it->second;
}

inline std::string region(std::string value, std::string_view provider) {
    // Friendly continental aliases are deterministic conveniences, not latency,
    // carbon, quota, or capacity optimisers.
    if (value == "europe")
        value = provider == "gcp" ? "europe-west4" : provider == "aws" ? "eu-west-1" : "westeurope";
    if (value == "us")
        value = provider == "gcp" ? "us-central1" : provider == "aws" ? "us-east-1" : "eastus";
    if (value == "asia")
        value = provider == "gcp"   ? "asia-east1"
                : provider == "aws" ? "ap-southeast-1"
                                    : "southeastasia";
    if (value.empty() || !gcp::detail::is_ascii_alpha(value.front()) ||
        !gcp::detail::is_ascii_alnum(value.back()))
        throw error("Invalid " + std::string(provider) + " region");
    for (const char c : value)
        if (!gcp::detail::is_ascii_lower(c) && !gcp::detail::is_ascii_digit(c) && c != '-')
            throw error("Invalid " + std::string(provider) + " region");
    return value;
}

inline void validate_project(std::string_view value) {
    if (value.empty())
        throw error("GCP project must not be empty");
    for (const char c : value)
        if (!gcp::detail::is_ascii_alnum(c) && c != '-' && c != '.' && c != ':')
            throw error("Invalid GCP project identifier");
}

inline bool configured_provider(const config& cfg, std::string_view wanted) {
    if (cfg.provider)
        return *cfg.provider == wanted;
    return std::find(cfg.providers.begin(), cfg.providers.end(), wanted) != cfg.providers.end();
}

inline std::string canonical_gpu(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), gcp::detail::ascii_lower);
    for (const std::string_view prefix : {std::string_view("nvidia-"), std::string_view("tesla-")})
        if (gcp::detail::starts_with(value, prefix))
            value.erase(0, prefix.size());
    if (value == "t4" || value == "l4" || value == "a10" || value == "a100" || value == "h100")
        return value;
    throw error("Unsupported accelerator \"" + value + "\"; use t4, l4, a10, a100, or h100");
}

// Environment-backed construction deliberately has a narrow contract. It
// supplies infrastructure and credentials to the same provider-neutral client;
// job definitions remain ordinary job_spec values.
inline unsigned positive_environment_integer(std::string_view name) {
    const std::string text = gcp::detail::env(name);
    unsigned value = 0;
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || code != std::errc{} || end != text.data() + text.size() || value == 0)
        throw error(std::string(name) + " must be a positive integer");
    return value;
}

// Floating-point from_chars is part of C++17, but arrived late in several
// otherwise useful C++17 standard libraries. Validate the same narrow decimal
// grammar first, then parse with the fixed classic locale supplied since C++98.
inline std::optional<double> parse_decimal(std::string_view text) {
    std::size_t position = 0;
    if (position < text.size() && text[position] == '-')
        ++position;
    bool has_digit = false;
    bool has_nonzero_digit = false;
    while (position < text.size() && gcp::detail::is_ascii_digit(text[position])) {
        has_digit = true;
        has_nonzero_digit = has_nonzero_digit || text[position] != '0';
        ++position;
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        while (position < text.size() && gcp::detail::is_ascii_digit(text[position])) {
            has_digit = true;
            has_nonzero_digit = has_nonzero_digit || text[position] != '0';
            ++position;
        }
    }
    if (!has_digit)
        return std::nullopt;
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-'))
            ++position;
        const std::size_t exponent = position;
        while (position < text.size() && gcp::detail::is_ascii_digit(text[position]))
            ++position;
        if (position == exponent)
            return std::nullopt;
    }
    if (position != text.size())
        return std::nullopt;

    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    double value = 0;
    input >> std::noskipws >> value;
    // Stream range reporting for subnormal values varied across early C++17
    // libraries. Such prices and resource quantities are not useful here, so
    // reject every range failure and subnormal consistently.
    if (!input || input.rdbuf()->sgetc() != std::char_traits<char>::eof() ||
        !std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL ||
        (value == 0 && has_nonzero_digit))
        return std::nullopt;
    return value;
}

inline double positive_environment_decimal(std::string_view name) {
    const std::string text = gcp::detail::env(name);
    const auto value = parse_decimal(text);
    if (!value || !(*value > 0))
        throw error(std::string(name) + " must be a positive decimal number");
    return *value;
}

inline void environment_locations(config& out) {
    if (const std::string value = gcp::detail::env("CLDMUX_REGION"); !value.empty())
        out.region = value;
    if (const std::string value = gcp::detail::env("CLDMUX_ZONE"); !value.empty())
        out.zone = value;
    if (const std::string value = gcp::detail::env("CLDMUX_GCP_REGION"); !value.empty())
        out.regions["gcp"] = value;
    if (const std::string value = gcp::detail::env("CLDMUX_AWS_REGION"); !value.empty())
        out.regions["aws"] = value;
    if (const std::string value = gcp::detail::env("CLDMUX_AZURE_REGION"); !value.empty())
        out.regions["azure"] = value;
    if (const std::string value = gcp::detail::env("CLDMUX_GCP_ZONE"); !value.empty())
        out.zones["gcp"] = value;
    if (const std::string value = gcp::detail::env("CLDMUX_AWS_ZONE"); !value.empty())
        out.zones["aws"] = value;
}

inline bool environment_aws_configured() {
    // Storage-only and raw-instance-only settings do not make Batch runnable.
    // Fargate queues are included because mounted CPU jobs have a composable
    // public-catalogue vCPU and memory price.
    return !gcp::detail::env("CLDMUX_AWS_JOB_QUEUE").empty() ||
           !gcp::detail::env("CLDMUX_AWS_SPOT_JOB_QUEUE").empty() ||
           !gcp::detail::env("CLDMUX_AWS_FARGATE_JOB_QUEUE").empty() ||
           !gcp::detail::env("CLDMUX_AWS_FARGATE_SPOT_JOB_QUEUE").empty() ||
           !gcp::detail::env("CLDMUX_AWS_GPU_MODEL").empty() ||
           !gcp::detail::env("CLDMUX_AWS_GPU_JOB_QUEUE").empty() ||
           !gcp::detail::env("CLDMUX_AWS_GPU_SPOT_JOB_QUEUE").empty() ||
           !gcp::detail::env("CLDMUX_AWS_GPU_MACHINE_TYPE").empty() ||
           !gcp::detail::env("CLDMUX_AWS_GPU_CPUS").empty() ||
           !gcp::detail::env("CLDMUX_AWS_GPU_MEMORY_GB").empty() ||
           !gcp::detail::env("CLDMUX_AWS_GPU_COUNT").empty();
}

inline std::string environment_compute_template() {
    return gcp::detail::env("CLDMUX_COMPUTE_TEMPLATE");
}

inline void require_environment_template(std::string_view native_name,
                                         const std::string& logical) {
    if (logical.empty())
        throw error(std::string(native_name) + " requires CLDMUX_COMPUTE_TEMPLATE");
}

inline void environment_aws_gpu_target(config& out) {
    const std::string model = gcp::detail::env("CLDMUX_AWS_GPU_MODEL");
    const std::string queue = gcp::detail::env("CLDMUX_AWS_GPU_JOB_QUEUE");
    const std::string spot_queue = gcp::detail::env("CLDMUX_AWS_GPU_SPOT_JOB_QUEUE");
    const std::string machine = gcp::detail::env("CLDMUX_AWS_GPU_MACHINE_TYPE");
    const std::string cpus = gcp::detail::env("CLDMUX_AWS_GPU_CPUS");
    const std::string memory = gcp::detail::env("CLDMUX_AWS_GPU_MEMORY_GB");
    const std::string count = gcp::detail::env("CLDMUX_AWS_GPU_COUNT");
    const bool present = !model.empty() || !queue.empty() || !spot_queue.empty() ||
                         !machine.empty() || !cpus.empty() || !memory.empty() || !count.empty();
    if (!present)
        return;
    if (model.empty() || machine.empty() || cpus.empty() || memory.empty() || count.empty() ||
        (queue.empty() && spot_queue.empty()))
        throw error("AWS GPU environment configuration requires a model, machine type, CPU, "
                    "memory, GPU count, and at least one queue");
    const std::string canonical = canonical_gpu(model);
    aws_gpu_target target;
    target.job_queue = queue;
    target.spot_job_queue = spot_queue;
    target.machine_type = machine;
    target.cpus = positive_environment_integer("CLDMUX_AWS_GPU_CPUS");
    target.memory_gb = positive_environment_decimal("CLDMUX_AWS_GPU_MEMORY_GB");
    target.gpus = positive_environment_integer("CLDMUX_AWS_GPU_COUNT");
    out.aws.gpu_targets[canonical] = std::move(target);
}

inline void environment_gcp(config& out) {
    out.project = gcp::detail::env("CLDMUX_GCP_PROJECT");
    if (out.project.empty())
        throw error("GCP environment configuration requires CLDMUX_GCP_PROJECT");
    validate_project(out.project);
    const std::string native = gcp::detail::env("CLDMUX_GCP_INSTANCE_TEMPLATE");
    if (!native.empty()) {
        const std::string logical = environment_compute_template();
        require_environment_template("CLDMUX_GCP_INSTANCE_TEMPLATE", logical);
        out.instance_templates[logical].gcp_instance_template = native;
    }
}

inline void environment_aws_s3_files(config& out, std::string_view role) {
    std::string prefix = "CLDMUX_AWS_S3_FILES";
    if (!role.empty())
        prefix += '_' + std::string(role);
    const std::string bucket = gcp::detail::env(prefix + "_BUCKET");
    const std::string file_system = gcp::detail::env(prefix + "_FILE_SYSTEM_ARN");
    const std::string access_point = gcp::detail::env(prefix + "_ACCESS_POINT_ARN");
    if (bucket.empty() && file_system.empty() && access_point.empty())
        return;
    if (bucket.empty() || file_system.empty())
        throw error(prefix + " environment configuration requires a bucket and file system ARN");
    const aws_s3_files_mount value{file_system, access_point};
    const auto [found, inserted] = out.aws.s3_files.emplace(bucket, value);
    if (!inserted && (found->second.file_system_arn != value.file_system_arn ||
                      found->second.access_point_arn != value.access_point_arn))
        throw error("Conflicting AWS S3 Files environment mappings for bucket " + bucket);
}

inline void environment_aws(config& out, bool comparing_prices) {
    out.aws.job_queue = gcp::detail::env("CLDMUX_AWS_JOB_QUEUE");
    out.aws.spot_job_queue = gcp::detail::env("CLDMUX_AWS_SPOT_JOB_QUEUE");
    out.aws.fargate_job_queue = gcp::detail::env("CLDMUX_AWS_FARGATE_JOB_QUEUE");
    out.aws.fargate_spot_job_queue =
        gcp::detail::env("CLDMUX_AWS_FARGATE_SPOT_JOB_QUEUE");
    out.aws.execution_role_arn = gcp::detail::env("CLDMUX_AWS_EXECUTION_ROLE_ARN");
    out.aws.job_role_arn = gcp::detail::env("CLDMUX_AWS_JOB_ROLE_ARN");
    out.aws.machine_type = gcp::detail::env("CLDMUX_AWS_MACHINE_TYPE");
    out.aws.spot_machine_type = gcp::detail::env("CLDMUX_AWS_SPOT_MACHINE_TYPE");
    if (const std::string value = gcp::detail::env("CLDMUX_AWS_LOG_GROUP"); !value.empty())
        out.aws.log_group = value;
    environment_aws_gpu_target(out);

    environment_aws_s3_files(out, {});
    environment_aws_s3_files(out, "INPUT");
    environment_aws_s3_files(out, "OUTPUT");

    const std::string launch_id = gcp::detail::env("CLDMUX_AWS_LAUNCH_TEMPLATE_ID");
    const std::string launch_name = gcp::detail::env("CLDMUX_AWS_LAUNCH_TEMPLATE_NAME");
    const std::string launch_version = gcp::detail::env("CLDMUX_AWS_LAUNCH_TEMPLATE_VERSION");
    if (!launch_id.empty() || !launch_name.empty() || !launch_version.empty()) {
        const std::string logical = environment_compute_template();
        require_environment_template("AWS launch-template configuration", logical);
        if (launch_id.empty() == launch_name.empty())
            throw error("AWS raw compute requires exactly one of "
                        "CLDMUX_AWS_LAUNCH_TEMPLATE_ID and CLDMUX_AWS_LAUNCH_TEMPLATE_NAME");
        auto& target = out.instance_templates[logical].aws;
        target.id = launch_id;
        target.name = launch_name;
        if (!launch_version.empty())
            target.version = launch_version;
    }
    const std::string zone = configured_zone(out, "aws");
    const bool priced_cpu = !out.aws.job_queue.empty() && !out.aws.machine_type.empty();
    const bool priced_spot =
        !out.aws.spot_job_queue.empty() && !out.aws.spot_machine_type.empty() && !zone.empty();
    const bool priced_gpu =
        std::any_of(out.aws.gpu_targets.begin(), out.aws.gpu_targets.end(), [&](const auto& item) {
            return !item.second.job_queue.empty() ||
                   (!item.second.spot_job_queue.empty() && !zone.empty());
        });
    const bool fargate =
        !out.aws.fargate_job_queue.empty() || !out.aws.fargate_spot_job_queue.empty();
    if (comparing_prices && !priced_cpu && !priced_spot && !priced_gpu && !fargate)
        throw error("AWS price comparison requires an exact EC2 machine queue or a Fargate "
                    "queue; EC2 Spot pricing also requires CLDMUX_AWS_ZONE");
    if (comparing_prices && (gcp::detail::env("AWS_ACCESS_KEY_ID").empty() ||
                             gcp::detail::env("AWS_SECRET_ACCESS_KEY").empty()))
        throw error("AWS price comparison requires AWS_ACCESS_KEY_ID and "
                    "AWS_SECRET_ACCESS_KEY");
}

inline void validate_environment_azure_endpoint(const config& out, std::string endpoint) {
    constexpr std::string_view scheme = "https://";
    if (!gcp::detail::starts_with(endpoint, scheme))
        throw error("CLDMUX_AZURE_BATCH_ENDPOINT must use HTTPS");
    endpoint.erase(0, scheme.size());
    std::transform(endpoint.begin(), endpoint.end(), endpoint.begin(), gcp::detail::ascii_lower);
    const std::string azure_region = region(configured_region(out, "azure"), "azure");
    const std::string suffix = "." + azure_region + ".batch.azure.com";
    if (!gcp::detail::ends_with(endpoint, suffix) || endpoint.size() <= suffix.size())
        throw error("CLDMUX_AZURE_BATCH_ENDPOINT must be "
                    "https://ACCOUNT.REGION.batch.azure.com");
    endpoint.resize(endpoint.size() - suffix.size());
    if (endpoint.size() < 3 || endpoint.size() > 24 ||
        !std::all_of(endpoint.begin(), endpoint.end(), [](char c) {
            return gcp::detail::is_ascii_lower(c) || gcp::detail::is_ascii_digit(c);
        }))
        throw error("CLDMUX_AZURE_BATCH_ENDPOINT contains an invalid Batch account name");
}

inline void environment_azure(config& out) {
    out.azure.batch_endpoint =
        gcp::detail::base_url(gcp::detail::env("CLDMUX_AZURE_BATCH_ENDPOINT"));
    // Restrict bearer-token destinations in the minimal environment contract.
    // Explicit cldmux::config remains the escape hatch for a trusted proxy or a
    // sovereign-cloud endpoint with a different DNS suffix.
    if (!out.azure.batch_endpoint.empty())
        validate_environment_azure_endpoint(out, out.azure.batch_endpoint);

    out.azure.storage_account = gcp::detail::env("CLDMUX_AZURE_STORAGE_ACCOUNT");
    if (!out.azure.storage_account.empty()) {
        if (out.azure.storage_account.size() < 3 || out.azure.storage_account.size() > 24 ||
            !std::all_of(out.azure.storage_account.begin(), out.azure.storage_account.end(),
                         [](char c) {
                             return gcp::detail::is_ascii_lower(c) ||
                                    gcp::detail::is_ascii_digit(c);
                         }))
            throw error("CLDMUX_AZURE_STORAGE_ACCOUNT must contain 3-24 lowercase letters or "
                        "digits");
        out.azure.storage_endpoint =
            "https://" + out.azure.storage_account + ".blob.core.windows.net";
    }
    out.azure.storage_sas = gcp::detail::env("CLDMUX_AZURE_STORAGE_SAS");
    if (!out.azure.storage_sas.empty() && out.azure.storage_sas.front() == '?')
        out.azure.storage_sas.erase(out.azure.storage_sas.begin());

    out.azure.subscription_id = gcp::detail::env("CLDMUX_AZURE_SUBSCRIPTION_ID");
    out.azure.resource_group = gcp::detail::env("CLDMUX_AZURE_RESOURCE_GROUP");
    const std::string image = gcp::detail::env("CLDMUX_AZURE_VM_IMAGE_ID");
    const std::string subnet = gcp::detail::env("CLDMUX_AZURE_VM_SUBNET_ID");
    const std::string size = gcp::detail::env("CLDMUX_AZURE_VM_SIZE");
    const std::string location = gcp::detail::env("CLDMUX_AZURE_VM_LOCATION");
    const std::string disk = gcp::detail::env("CLDMUX_AZURE_VM_OS_DISK_TYPE");
    if (!image.empty() || !subnet.empty() || !size.empty() || !location.empty() ||
        !disk.empty()) {
        const std::string logical = environment_compute_template();
        require_environment_template("Azure VM template configuration", logical);
        if (image.empty() || subnet.empty() || size.empty())
            throw error("Azure raw compute requires CLDMUX_AZURE_VM_IMAGE_ID, "
                        "CLDMUX_AZURE_VM_SUBNET_ID, and CLDMUX_AZURE_VM_SIZE");
        auto& target = out.instance_templates[logical].azure;
        target.image_id = image;
        target.subnet_id = subnet;
        target.machine_type = size;
        target.location = location;
        if (!disk.empty())
            target.os_disk_type = disk;
    }
    const bool raw_configured = !image.empty() || !out.azure.subscription_id.empty() ||
                                !out.azure.resource_group.empty();
    if (raw_configured && (out.azure.subscription_id.empty() || out.azure.resource_group.empty()))
        throw error("Azure raw compute requires CLDMUX_AZURE_SUBSCRIPTION_ID and "
                    "CLDMUX_AZURE_RESOURCE_GROUP");
    if (out.azure.batch_endpoint.empty() && out.azure.storage_account.empty() && !raw_configured)
        throw error("Azure environment configuration requires Batch, Blob Storage, or raw VM "
                    "settings");

    // Tokens are fetched lazily and selected by the exact Azure audience. A
    // zero expiry receives the conservative refresh lifetime used by the
    // shared credential cache.
    const auto environment_auth = auth::from([](std::string_view scope) {
        std::string variable;
        if (scope == "https://storage.azure.com/.default")
            variable = "CLDMUX_AZURE_STORAGE_TOKEN";
        else if (scope == "https://management.azure.com/.default")
            variable = "CLDMUX_AZURE_MANAGEMENT_TOKEN";
        else
            variable = "CLDMUX_AZURE_BATCH_TOKEN";
        std::string token = gcp::detail::env(variable);
        if (token.empty())
            throw error(variable + " is required for this Azure operation");
        return access_token{std::move(token), {}, {}};
    });
    out.azure.auth = environment_auth;
    out.azure.storage_auth = environment_auth;
    out.azure.management_auth = environment_auth;
}

inline config config_from_environment(std::string_view requested) {
    config out;
    environment_locations(out);

    if (requested == "gcp") {
        environment_gcp(out);
    } else if (requested == "aws") {
        environment_aws(out, false);
    } else if (requested == "azure") {
        environment_azure(out);
    } else if (requested == "cheapest") {
        out.providers.clear();
        out.selection = selection::lowest_cost;
        out.prices = price_source::public_catalogue;
        if (!gcp::detail::env("CLDMUX_GCP_PROJECT").empty()) {
            environment_gcp(out);
            out.providers.push_back("gcp");
        }
        if (environment_aws_configured()) {
            environment_aws(out, true);
            out.providers.push_back("aws");
        }
        if (!gcp::detail::env("CLDMUX_AZURE_BATCH_ENDPOINT").empty()) {
            environment_azure(out);
            out.providers.push_back("azure");
        }
        if (out.providers.size() < 2)
            throw error("cheapest routing requires at least two configured cloud providers");
        return out;
    } else {
        throw error("Unknown cloud provider \"" + std::string(requested) +
                    "\"; use cheapest, gcp, aws, or azure");
    }

    out.provider = std::string(requested);
    return out;
}

struct machine_choice {
    std::string name;
    std::string accelerator;
    unsigned accelerator_count = 0;
};

struct shape {
    const char* name;
    unsigned cpus;
    double memory;
};

template <std::size_t Size>
inline std::string smallest_shape(const resources& requested, const shape (&shapes)[Size],
                                  std::string_view provider) {
    for (const auto& candidate : shapes)
        if (requested.cpus <= candidate.cpus && requested.memory_gb <= candidate.memory)
            return candidate.name;
    throw error("No built-in " + std::string(provider) + " machine mapping satisfies the request");
}

inline machine_choice machine(std::string_view provider, const config& cfg,
                              const resources& requested, std::vector<std::string>& warnings) {
    // Built-in tables select the smallest shape satisfying CPU/RAM while keeping
    // the requested accelerator exact. AWS delegates capacity to preconfigured
    // queues, so GPU entries must declare one trustworthy native instance type.
    if (!requested.cpus || !(requested.memory_gb > 0) || !std::isfinite(requested.memory_gb))
        throw error("Resources require positive CPU and memory values");
    if (requested.gpu.empty()) {
        if (provider == "gcp") {
            static constexpr shape shapes[] = {
                {"e2-standard-2", 2, 8},    {"e2-standard-4", 4, 16},    {"e2-standard-8", 8, 32},
                {"e2-standard-16", 16, 64}, {"e2-standard-32", 32, 128},
            };
            return {smallest_shape(requested, shapes, provider), {}, 0};
        }
        if (provider == "azure") {
            static constexpr shape shapes[] = {
                {"Standard_D2s_v5", 2, 8},     {"Standard_D4s_v5", 4, 16},
                {"Standard_D8s_v5", 8, 32},    {"Standard_D16s_v5", 16, 64},
                {"Standard_D32s_v5", 32, 128},
            };
            return {smallest_shape(requested, shapes, provider), {}, 0};
        }
        const std::string exact = requested.spot ? cfg.aws.spot_machine_type : cfg.aws.machine_type;
        if (exact.empty()) {
            warnings.push_back("AWS Batch queue may choose several EC2 types; exact catalogue "
                               "pricing is unavailable");
            return {"batch-managed", {}, 0};
        }
        warnings.push_back("AWS machine type relies on the configured queue containing only " +
                           exact + " instances");
        return {exact, {}, 0};
    }

    if (!requested.gpu_count)
        throw error("Accelerator count must be positive");
    const std::string gpu = canonical_gpu(requested.gpu);
    if (provider == "gcp") {
        if (gpu == "t4" &&
            (requested.gpu_count == 1 || requested.gpu_count == 2 || requested.gpu_count == 4)) {
            static constexpr shape small[] = {
                {"n1-standard-4", 4, 15},
                {"n1-standard-8", 8, 30},
                {"n1-standard-16", 16, 60},
                {"n1-standard-32", 32, 120},
            };
            static constexpr shape large[] = {
                {"n1-standard-4", 4, 15},    {"n1-standard-8", 8, 30},
                {"n1-standard-16", 16, 60},  {"n1-standard-32", 32, 120},
                {"n1-standard-64", 64, 240}, {"n1-standard-96", 96, 360},
            };
            const std::string name = requested.gpu_count < 4
                                         ? smallest_shape(requested, small, provider)
                                         : smallest_shape(requested, large, provider);
            return {name, gpu, requested.gpu_count};
        }
        if (gpu == "l4" && requested.gpu_count == 1) {
            static constexpr shape one_gpu[] = {
                {"g2-standard-4", 4, 16},   {"g2-standard-8", 8, 32},    {"g2-standard-12", 12, 48},
                {"g2-standard-16", 16, 64}, {"g2-standard-32", 32, 128},
            };
            return {smallest_shape(requested, one_gpu, provider), gpu, 1};
        }
        if (gpu == "l4") {
            struct fixed {
                unsigned count;
                const char* name;
                unsigned cpus;
                double memory;
            };
            static constexpr fixed shapes[] = {
                {2, "g2-standard-24", 24, 96},
                {4, "g2-standard-48", 48, 192},
                {8, "g2-standard-96", 96, 384},
            };
            for (const auto& candidate : shapes)
                if (requested.gpu_count == candidate.count && requested.cpus <= candidate.cpus &&
                    requested.memory_gb <= candidate.memory)
                    return {candidate.name, gpu, candidate.count};
        }
        if (gpu == "a100") {
            struct fixed {
                unsigned count;
                const char* name;
                unsigned cpus;
                double memory;
            };
            static constexpr fixed shapes[] = {
                {1, "a2-ultragpu-1g", 12, 170},
                {2, "a2-ultragpu-2g", 24, 340},
                {4, "a2-ultragpu-4g", 48, 680},
                {8, "a2-ultragpu-8g", 96, 1360},
            };
            for (const auto& candidate : shapes)
                if (requested.gpu_count == candidate.count && requested.cpus <= candidate.cpus &&
                    requested.memory_gb <= candidate.memory)
                    return {candidate.name, gpu, candidate.count};
        }
        if (gpu == "h100") {
            struct fixed {
                unsigned count;
                const char* name;
                unsigned cpus;
                double memory;
            };
            static constexpr fixed shapes[] = {
                {1, "a3-highgpu-1g", 26, 234},
                {2, "a3-highgpu-2g", 52, 468},
                {4, "a3-highgpu-4g", 104, 936},
                {8, "a3-highgpu-8g", 208, 1872},
            };
            for (const auto& candidate : shapes)
                if (requested.gpu_count == candidate.count && requested.cpus <= candidate.cpus &&
                    requested.memory_gb <= candidate.memory) {
                    if (candidate.count < 8 && !requested.spot)
                        throw error(
                            "GCP A3 High shapes with fewer than eight H100 GPUs require Spot");
                    return {candidate.name, gpu, candidate.count};
                }
        }
        throw error(
            "No built-in GCP machine has the requested accelerator model, count, CPU, and memory");
    }
    if (provider == "azure") {
        if (requested.gpu_count != 1)
            throw error("Built-in Azure accelerator mappings currently require gpu_count == 1");
        struct gpu_shape {
            const char* gpu;
            const char* name;
            unsigned cpus;
            double memory;
        };
        static constexpr gpu_shape shapes[] = {
            {"t4", "Standard_NC4as_T4_v3", 4, 28},
            {"a10", "Standard_NV36ads_A10_v5", 36, 440},
            {"a100", "Standard_NC24ads_A100_v4", 24, 220},
            {"h100", "Standard_NC40ads_H100_v5", 40, 320},
        };
        for (const auto& candidate : shapes)
            if (gpu == candidate.gpu && requested.cpus <= candidate.cpus &&
                requested.memory_gb <= candidate.memory)
                return {candidate.name, gpu, 1};
        throw error("No built-in Azure machine has the requested accelerator model, count, CPU, "
                    "and memory");
    }

    const auto target = cfg.aws.gpu_targets.find(gpu);
    if (target == cfg.aws.gpu_targets.end() || target->second.machine_type.empty() ||
        (requested.spot ? target->second.spot_job_queue : target->second.job_queue).empty())
        throw error("AWS accelerator \"" + gpu +
                    "\" requires a dedicated queue and exact instance type in config::aws");
    if (!target->second.cpus || !(target->second.memory_gb > 0) ||
        !std::isfinite(target->second.memory_gb) || !target->second.gpus)
        throw error("AWS accelerator target \"" + gpu + "\" has incomplete capacity metadata");
    if (requested.cpus > target->second.cpus || requested.memory_gb > target->second.memory_gb ||
        requested.gpu_count > target->second.gpus)
        throw error("AWS accelerator target \"" + gpu +
                    "\" cannot satisfy the requested resources");
    warnings.push_back("AWS GPU model relies on the configured queue containing only " + gpu +
                       " instances");
    return {target->second.machine_type, gpu, requested.gpu_count};
}

inline void validate_spec(const job_spec& spec);
inline void validate_provider_spec(std::string_view provider, const config& cfg,
                                   const job_spec& spec);
inline std::uint64_t memory_mib(double memory_gb);
inline std::uint64_t fargate_memory_mib(const resources& requested);

inline cldmux::plan make_provider_plan(const config& cfg, const job_spec& spec,
                                      std::string provider) {
    // Provider planning is deterministic and mutation-free. It validates both
    // portable and backend-specific contracts before exposing a native shape.
    validate_spec(spec);
    cldmux::plan out;
    out.provider = std::move(provider);
    if (!implemented(out.provider))
        throw error(out.provider + " backend is not implemented");
    validate_provider_spec(out.provider, cfg, spec);
    out.region = detail::region(configured_region(cfg, out.provider), out.provider);
    const auto selected_machine = out.provider == "aws" && !spec.mounts.empty()
                                      ? machine_choice{"FARGATE", {}, 0}
                                      : machine(out.provider, cfg, spec.resources, out.warnings);
    out.machine_type = selected_machine.name;
    out.accelerator = selected_machine.accelerator;
    out.accelerator_count = selected_machine.accelerator_count;
    if (spec.resources.max_price_per_hour) {
        if (!std::isfinite(*spec.resources.max_price_per_hour) ||
            *spec.resources.max_price_per_hour < 0)
            throw error("Maximum hourly price must be finite and nonnegative");
    }
    out.warnings.push_back("egress cost is not estimated");
    if (out.provider == "gcp" && spec.service_account.empty())
        out.warnings.push_back("Batch VM uses the default Compute Engine service account");
    if (out.provider == "aws")
        out.warnings.push_back("AWS Batch retains terminal job records; auto_delete removes only "
                               "the temporary job definition");
    if (out.provider == "aws")
        out.warnings.push_back(
            "AWS Batch command replaces image CMD but preserves the image ENTRYPOINT");
    if (out.provider == "aws" && !spec.mounts.empty()) {
        const auto rounded = fargate_memory_mib(spec.resources);
        if (rounded != memory_mib(spec.resources.memory_gb))
            out.warnings.push_back("AWS Fargate memory was rounded up to " +
                                   std::to_string(rounded) + " MiB");
        out.warnings.push_back("AWS S3 Files mounts use Fargate and cannot attach GPUs");
        out.warnings.push_back("AWS Fargate bills from image download with a one-minute minimum");
    }
    if (out.provider == "azure")
        out.warnings.push_back(
            "Azure task-file logs are grouped by stdout/stderr rather than globally ordered");
    return out;
}

inline std::string strings(const std::vector<std::string>& values, std::size_t begin = 0) {
    std::string out = "[";
    for (std::size_t i = begin; i < values.size(); ++i) {
        if (i != begin)
            out += ',';
        out += gcp::detail::json_quote(values[i]);
    }
    return out + ']';
}

inline void validate_workdir(std::string_view path) {
    for (const char c : path)
        if (!gcp::detail::is_ascii_alnum(c) && c != '/' && c != '.' && c != '_' && c != '-')
            throw error("Container workdir contains an unsafe character");
}

inline std::uint64_t memory_mib(double memory_gb) {
    const long double value = std::ceil(static_cast<long double>(memory_gb) * 1024.0L);
    if (!std::isfinite(value) || value < 1 ||
        value > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        throw error("Requested memory cannot be represented in MiB");
    return static_cast<std::uint64_t>(value);
}

inline std::uint64_t fargate_memory_mib(const resources& requested) {
    const std::uint64_t wanted = memory_mib(requested.memory_gb);
    struct fargate_shape {
        unsigned cpus;
        std::uint64_t first;
        std::uint64_t last;
        std::uint64_t step;
    };
    static constexpr fargate_shape shapes[] = {
        {1, 2 * 1024, 8 * 1024, 1024},       {2, 4 * 1024, 16 * 1024, 1024},
        {4, 8 * 1024, 30 * 1024, 1024},      {8, 16 * 1024, 60 * 1024, 4 * 1024},
        {16, 32 * 1024, 120 * 1024, 8 * 1024},
    };
    for (const auto& shape : shapes) {
        if (requested.cpus != shape.cpus)
            continue;
        if (wanted > shape.last)
            break;
        if (wanted <= shape.first)
            return shape.first;
        const auto increments = (wanted - shape.first + shape.step - 1) / shape.step;
        return shape.first + increments * shape.step;
    }
    throw error("AWS Batch Fargate mounts require 1, 2, 4, 8, or 16 vCPUs and a supported "
                "memory quantity");
}

inline void validate_spec(const job_spec& spec) {
    if (spec.image.empty() || spec.command.empty() || spec.command.front().empty())
        throw error("A job requires a container image and a non-empty command");
    if (spec.retries > 10)
        throw error("Cloud jobs allow at most 10 retries");
    if (spec.timeout <= std::chrono::milliseconds::zero() ||
        spec.timeout > std::chrono::hours(24 * 14))
        throw error("Job timeout must be positive and at most 14 days");
    validate_workdir(spec.workdir);
    if (spec.service_account.find('\r') != std::string::npos ||
        spec.service_account.find('\n') != std::string::npos)
        throw error("Service account contains an invalid newline");
    for (const auto& item : spec.mounts) {
        (void)parse_uri(item.source);
        if (item.target.empty() || item.target.front() != '/' ||
            item.target.find(':') != std::string::npos)
            throw error("Mount targets must be absolute container paths without ':'");
        for (const char c : item.target)
            if (!gcp::detail::is_ascii_alnum(c) && c != '/' && c != '.' && c != '_' && c != '-')
                throw error("Mount targets contain an unsafe character");
        std::size_t begin = 1;
        while (begin <= item.target.size()) {
            const auto slash = item.target.find('/', begin);
            const std::size_t length =
                slash == std::string::npos ? item.target.size() - begin : slash - begin;
            const std::string_view component(item.target.data() + begin, length);
            if (component == "." || component == "..")
                throw error("Mount targets must not contain '.' or '..' path components");
            if (slash == std::string::npos)
                break;
            begin = slash + 1;
        }
    }
}

inline void validate_provider_spec(std::string_view provider, const config& cfg,
                                   const job_spec& spec) {
    if (provider == "gcp") {
        for (const auto& item : spec.mounts) {
            const uri source = parse_uri(item.source);
            if (!source.key.empty() && !gcp::detail::ends_with(source.key, "/"))
                throw error("GCS job mounts require a bucket or directory prefix ending in '/'");
        }
        return;
    }
    if (provider == "aws") {
        if (!spec.workdir.empty())
            throw error("AWS Batch has no portable per-job container working-directory field");
        if (spec.retries > 9)
            throw error("AWS Batch permits at most 10 total attempts");
        if (spec.timeout < std::chrono::seconds(60))
            throw error("AWS Batch job timeout must be at least 60 seconds");
        if (memory_mib(spec.resources.memory_gb) < 4)
            throw error("AWS Batch EC2 jobs require at least 4 MiB of memory");
        const std::string gpu =
            spec.resources.gpu.empty() ? std::string{} : canonical_gpu(spec.resources.gpu);
        if (!spec.mounts.empty()) {
            if (!gpu.empty())
                throw error("AWS S3 Files mounts use Fargate, which does not support GPUs");
            const auto& queue = spec.resources.spot ? cfg.aws.fargate_spot_job_queue
                                                    : cfg.aws.fargate_job_queue;
            if (queue.empty())
                throw error(std::string("AWS mounted ") +
                            (spec.resources.spot ? "Fargate Spot" : "Fargate") +
                            " jobs require a configured queue");
            if (cfg.aws.execution_role_arn.empty())
                throw error("AWS mounted jobs require config::aws.execution_role_arn");
            if (spec.service_account.empty() && cfg.aws.job_role_arn.empty())
                throw error("AWS mounted jobs require a job role ARN");
            (void)fargate_memory_mib(spec.resources);
            const std::string selected_region =
                region(configured_region(cfg, "aws"), "aws");
            for (const auto& item : spec.mounts) {
                const uri source = parse_uri(item.source);
                if (!source.key.empty() && !gcp::detail::ends_with(source.key, "/"))
                    throw error("AWS S3 Files mounts require a bucket or directory prefix ending "
                                "in '/'");
                const auto mapping = cfg.aws.s3_files.find(source.bucket);
                if (mapping == cfg.aws.s3_files.end() || mapping->second.file_system_arn.empty())
                    throw error("AWS S3 Files has no configured file system for bucket " +
                                source.bucket);
                if (mapping->second.file_system_arn.find(":s3files:" + selected_region + ':') ==
                    std::string::npos)
                    throw error("AWS S3 Files ARN region does not match the selected AWS region");
                if (!mapping->second.access_point_arn.empty() && !source.key.empty())
                    throw error("AWS S3 Files access-point mounts cannot select a prefix");
            }
            return;
        }
        if (gpu.empty()) {
            const auto& queue = spec.resources.spot ? cfg.aws.spot_job_queue : cfg.aws.job_queue;
            if (queue.empty())
                throw error(std::string("AWS ") + (spec.resources.spot ? "Spot" : "on-demand") +
                            " jobs require a configured Batch queue");
        } else {
            const auto it = cfg.aws.gpu_targets.find(gpu);
            if (it == cfg.aws.gpu_targets.end() ||
                (spec.resources.spot ? it->second.spot_job_queue : it->second.job_queue).empty())
                throw error("AWS accelerator \"" + gpu +
                            "\" requires a configured dedicated queue");
        }
        return;
    }
    if (!spec.service_account.empty())
        throw error("Azure Batch auto-pools do not support job_spec::service_account");
    if (cfg.azure.batch_endpoint.empty())
        throw error("Azure jobs require config::azure.batch_endpoint");
    if (!gcp::detail::starts_with(cfg.azure.batch_endpoint, "https://") &&
        !(cfg.allow_insecure_http && gcp::detail::starts_with(cfg.azure.batch_endpoint, "http://")))
        throw error("Azure batch_endpoint must use HTTPS (or explicitly allow insecure HTTP)");
    std::string endpoint = cfg.azure.batch_endpoint;
    std::transform(endpoint.begin(), endpoint.end(), endpoint.begin(), gcp::detail::ascii_lower);
    const std::string azure_region = region(configured_region(cfg, "azure"), "azure");
    if (endpoint.find(".batch.") != std::string::npos &&
        endpoint.find(std::string(".") + azure_region + ".batch.") == std::string::npos)
        throw error("Azure batch_endpoint region does not match the selected Azure region");
    if (!spec.mounts.empty()) {
        if (spec.mounts.size() > 10)
            throw error("Azure Batch supports at most 10 virtual file-system mounts");
        if (cfg.azure.storage_account.empty() || cfg.azure.storage_sas.empty())
            throw error("Azure Blob mounts require a storage account and node SAS");
        std::string node_agent = cfg.azure.node_agent_sku;
        std::transform(node_agent.begin(), node_agent.end(), node_agent.begin(),
                       gcp::detail::ascii_lower);
        if (node_agent.find("windows") != std::string::npos)
            throw error("Azure Blob container mounts require a Linux Batch image");
        for (const auto& item : spec.mounts)
            if (!parse_uri(item.source).key.empty())
                throw error("Azure Blob mounts support complete containers, not prefixes");
    }
    for (const auto& argument : spec.command) {
        if (argument.empty())
            throw error("Azure Batch command arguments must not be empty");
        for (const char c : argument)
            if (!gcp::detail::is_ascii_alnum(c) && c != '/' && c != '.' && c != '_' && c != '-' &&
                c != ':' && c != '=' && c != '+' && c != '@' && c != '%' && c != ',')
                throw error(
                    "Azure Batch direct arguments currently allow only portable token characters");
    }
}

inline std::uint64_t duration_seconds(std::chrono::milliseconds value) {
    const auto milliseconds = value.count();
    return static_cast<std::uint64_t>(milliseconds / 1000 + (milliseconds % 1000 != 0));
}

inline std::uint64_t provider_attempt_timeout_seconds(const job_spec& spec) {
    return duration_seconds(spec.timeout);
}

inline std::uint64_t azure_job_timeout_seconds(const job_spec& spec, const config& cfg) {
    return provider_attempt_timeout_seconds(spec) + duration_seconds(cfg.cleanup_timeout) +
           duration_seconds(cfg.final_log_timeout) + duration_seconds(cfg.request_timeout);
}

inline std::string batch_body(const job_spec& spec, const cldmux::plan& chosen) {
    // GCP Batch owns queueing and temporary VM lifecycle. The request creates one
    // task, blocks inherited project SSH keys, mounts GCS prefixes through GCS
    // FUSE, and lets Batch install GPU drivers when an accelerator is present.
    std::string container = "{\"imageUri\":" + gcp::detail::json_quote(spec.image) +
                            ",\"entrypoint\":" + gcp::detail::json_quote(spec.command.front()) +
                            ",\"commands\":" + strings(spec.command, 1);
    if (!spec.workdir.empty())
        container += ",\"options\":" + gcp::detail::json_quote("--workdir=" + spec.workdir);

    std::string task_volumes;
    std::string container_volumes;
    for (std::size_t i = 0; i < spec.mounts.size(); ++i) {
        const auto& item = spec.mounts[i];
        const uri source = parse_uri(item.source);
        const std::string host = "/mnt/disks/cldmux-" + std::to_string(i);
        const std::string remote = source.bucket + (source.key.empty() ? "" : "/" + source.key);
        if (!task_volumes.empty()) {
            task_volumes += ',';
            container_volumes += ',';
        }
        task_volumes += "{\"gcs\":{\"remotePath\":" + gcp::detail::json_quote(remote) +
                        "},\"mountPath\":" + gcp::detail::json_quote(host) + '}';
        container_volumes +=
            gcp::detail::json_quote(host + ":" + item.target + (item.read_only ? ":ro" : ""));
    }
    if (!container_volumes.empty())
        container += ",\"volumes\":[" + container_volumes + ']';
    container += '}';

    const auto seconds = provider_attempt_timeout_seconds(spec);
    const auto memory = memory_mib(spec.resources.memory_gb);
    std::string task =
        "{\"runnables\":[{\"container\":" + container + "}],\"computeResource\":{\"cpuMilli\":" +
        gcp::detail::json_quote(std::to_string(spec.resources.cpus * 1000ULL)) +
        ",\"memoryMib\":" + gcp::detail::json_quote(std::to_string(memory)) +
        "},\"maxRunDuration\":" +
        gcp::detail::json_quote(std::to_string(seconds) + "s") +
        ",\"maxRetryCount\":" + std::to_string(spec.retries);
    if (!task_volumes.empty())
        task += ",\"volumes\":[" + task_volumes + ']';
    task += '}';

    const std::string service_account =
        spec.service_account.empty()
            ? std::string{}
            : ",\"serviceAccount\":{\"email\":" + gcp::detail::json_quote(spec.service_account) +
                  '}';

    std::string instance_policy =
        "{\"machineType\":" + gcp::detail::json_quote(chosen.machine_type) +
        ",\"provisioningModel\":\"" + std::string(spec.resources.spot ? "SPOT" : "STANDARD") + '"';
    if (chosen.accelerator == "t4")
        instance_policy += ",\"accelerators\":[{\"type\":\"nvidia-tesla-t4\",\"count\":" +
                           std::to_string(chosen.accelerator_count) + "}]";
    instance_policy += '}';
    const std::string gpu_driver =
        chosen.accelerator.empty() ? std::string{} : ",\"installGpuDrivers\":true";

    // Keep the historical cloud-hpp label keys stable because external cleanup
    // and inventory policies may already select them.
    return "{\"taskGroups\":[{\"taskSpec\":" + task +
           ",\"taskCount\":\"1\",\"parallelism\":\"1\"}],"
           "\"allocationPolicy\":{\"location\":{\"allowedLocations\":[" +
           gcp::detail::json_quote("regions/" + chosen.region) +
           "]},\"instances\":[{\"policy\":" + instance_policy + ",\"blockProjectSshKeys\":true" +
           gpu_driver + "}]" + service_account +
           "},"
           "\"logsPolicy\":{\"destination\":\"CLOUD_LOGGING\"},"
           "\"labels\":{\"cloud-hpp\":\"temporary\",\"cloud-hpp-ttl-seconds\":" +
           gcp::detail::json_quote(std::to_string(seconds)) + "}}";
}

inline std::string job_id(std::string value) {
    // Reserve space below provider length limits for a random suffix. The suffix
    // makes names unique audit/recovery handles for ambiguous submissions.
    for (char& c : value) {
        c = gcp::detail::ascii_lower(c);
        if (!gcp::detail::is_ascii_alnum(c))
            c = '-';
    }
    while (!value.empty() && value.back() == '-')
        value.pop_back();
    if (value.empty() || !gcp::detail::is_ascii_alpha(value.front()))
        value = "job-" + value;
    if (value.size() > 46)
        value.resize(46);
    std::string suffix = gcp::detail::random_uuid();
    suffix.erase(std::remove(suffix.begin(), suffix.end(), '-'), suffix.end());
    return value + '-' + suffix.substr(0, 16);
}

} // namespace detail
} // namespace cldmux
