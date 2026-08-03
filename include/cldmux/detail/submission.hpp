#pragma once

#include "cldmux/job.hpp"

namespace cldmux {
namespace detail {

// AWS and Azure submission -----------------------------------------------------

inline std::string aws_queue(const config& cfg, const job_spec& spec) {
    if (!spec.mounts.empty())
        return spec.resources.spot ? cfg.aws.fargate_spot_job_queue : cfg.aws.fargate_job_queue;
    if (spec.resources.gpu.empty())
        return spec.resources.spot ? cfg.aws.spot_job_queue : cfg.aws.job_queue;
    const std::string gpu = canonical_gpu(spec.resources.gpu);
    const auto it = cfg.aws.gpu_targets.find(gpu);
    if (it == cfg.aws.gpu_targets.end())
        throw error("Missing AWS queue for accelerator " + gpu);
    return spec.resources.spot ? it->second.spot_job_queue : it->second.job_queue;
}

inline std::vector<std::string>
find_aws_definitions(const client_state& client, std::string_view endpoint, std::string_view region,
                     std::string_view name, std::chrono::steady_clock::time_point deadline) {
    std::vector<std::string> found;
    std::string token;
    for (int page = 0; page < 100; ++page) {
        std::string body = "{\"jobDefinitionName\":" + gcp::detail::json_quote(name) +
                           ",\"status\":\"ACTIVE\",\"maxResults\":100";
        if (!token.empty())
            body += ",\"nextToken\":" + gcp::detail::json_quote(token);
        body += '}';
        const auto json = gcp::detail::parse_json(
            aws_call(client,
                     gcp::detail::HttpRequest{}
                         .with_method("POST")
                         .with_url(std::string(endpoint) + "/v1/describejobdefinitions")
                         .with_headers({"Content-Type: application/json"})
                         .with_body(std::move(body)),
                     std::string(region), "batch", true, deadline)
                .body);
        if (const auto* definitions = json.get("jobDefinitions"))
            for (const auto& item : definitions->array()) {
                if (gcp::detail::field(item, "jobDefinitionName") != name ||
                    gcp::detail::field(item, "status") != "ACTIVE")
                    continue;
                const std::string arn = gcp::detail::field(item, "jobDefinitionArn");
                if (arn.empty())
                    throw error("Malformed AWS Batch definition response for unique name " +
                                std::string(name));
                if (std::find(found.begin(), found.end(), arn) == found.end())
                    found.push_back(arn);
            }
        const std::string next = gcp::detail::field(json, "nextToken");
        if (next.empty())
            return found;
        if (next == token)
            throw error("AWS Batch definition pagination did not advance for unique name " +
                        std::string(name));
        token = next;
    }
    throw error("AWS Batch definition pagination exceeded 100 pages for unique name " +
                std::string(name));
}

inline std::optional<std::string> find_aws_job(const client_state& client,
                                               std::string_view endpoint, std::string_view region,
                                               std::string_view queue, std::string_view name,
                                               std::chrono::steady_clock::time_point deadline) {
    const std::string body = "{\"jobQueue\":" + gcp::detail::json_quote(queue) +
                             ",\"filters\":[{\"name\":\"JOB_NAME\",\"values\":[" +
                             gcp::detail::json_quote(name) + "]}],\"maxResults\":100}";
    const auto json =
        gcp::detail::parse_json(aws_call(client,
                                         gcp::detail::HttpRequest{}
                                             .with_method("POST")
                                             .with_url(std::string(endpoint) + "/v1/listjobs")
                                             .with_headers({"Content-Type: application/json"})
                                             .with_body(body),
                                         std::string(region), "batch", true, deadline)
                                    .body);
    const auto* jobs = json.get("jobSummaryList");
    std::optional<std::string> found;
    if (jobs)
        for (const auto& item : jobs->array()) {
            if (gcp::detail::field(item, "jobName") != name)
                continue;
            const std::string id = gcp::detail::field(item, "jobId");
            if (id.empty())
                continue;
            if (found && *found != id)
                throw error("AWS Batch returned several jobs for unique name " + std::string(name));
            found = id;
        }
    return found;
}

inline std::shared_ptr<job_data> submit_aws(std::shared_ptr<client_state> client,
                                            const job_spec& spec, const cldmux::plan& chosen,
                                            const std::string& name) {
    // Register a uniquely named temporary definition, then submit a uniquely
    // named job. A lost registration response is reconciled by exact-name lookup;
    // a lost submission response is reconciled by queue/name lookup. If submission
    // remains unknown, keep the definition active for a possibly accepted job.
    const bool fargate = !spec.mounts.empty();
    const auto memory =
        fargate ? fargate_memory_mib(spec.resources) : memory_mib(spec.resources.memory_gb);
    std::string requirements =
        "[{\"type\":\"VCPU\",\"value\":" +
        gcp::detail::json_quote(std::to_string(spec.resources.cpus)) +
        "},{\"type\":\"MEMORY\",\"value\":" + gcp::detail::json_quote(std::to_string(memory)) + '}';
    if (!chosen.accelerator.empty())
        requirements += ",{\"type\":\"GPU\",\"value\":" +
                        gcp::detail::json_quote(std::to_string(chosen.accelerator_count)) + '}';
    requirements += ']';
    std::string container = "{\"image\":" + gcp::detail::json_quote(spec.image) +
                            ",\"command\":" + strings(spec.command) +
                            ",\"resourceRequirements\":" + requirements;
    const std::string job_role =
        spec.service_account.empty() ? client->config.aws.job_role_arn : spec.service_account;
    if (!job_role.empty())
        container += ",\"jobRoleArn\":" + gcp::detail::json_quote(job_role);
    if (fargate) {
        container += ",\"executionRoleArn\":" +
                     gcp::detail::json_quote(client->config.aws.execution_role_arn) +
                     ",\"fargatePlatformConfiguration\":{\"platformVersion\":\"LATEST\"}";
        std::string volumes;
        std::string mount_points;
        for (std::size_t i = 0; i < spec.mounts.size(); ++i) {
            const auto& item = spec.mounts[i];
            const uri source = parse_uri(item.source);
            const auto& mapping = client->config.aws.s3_files.at(source.bucket);
            const std::string volume = "cldmux-" + std::to_string(i);
            if (!volumes.empty()) {
                volumes += ',';
                mount_points += ',';
            }
            std::string native = "{\"fileSystemArn\":" +
                                 gcp::detail::json_quote(mapping.file_system_arn);
            if (!mapping.access_point_arn.empty()) {
                native += ",\"accessPointArn\":" +
                          gcp::detail::json_quote(mapping.access_point_arn);
            } else {
                const std::string root = source.key.empty() ? "/" : "/" + source.key;
                native += ",\"rootDirectory\":" + gcp::detail::json_quote(root);
            }
            native += '}';
            volumes += "{\"name\":" + gcp::detail::json_quote(volume) +
                       ",\"s3filesVolumeConfiguration\":" + native + '}';
            mount_points += "{\"sourceVolume\":" + gcp::detail::json_quote(volume) +
                            ",\"containerPath\":" + gcp::detail::json_quote(item.target) +
                            ",\"readOnly\":" + (item.read_only ? "true" : "false") + '}';
        }
        container += ",\"volumes\":[" + volumes + "],\"mountPoints\":[" + mount_points + ']';
    }
    container += ",\"logConfiguration\":{\"logDriver\":\"awslogs\",\"options\":{"
                 "\"awslogs-group\":" +
                 gcp::detail::json_quote(client->config.aws.log_group) +
                 ",\"awslogs-region\":" + gcp::detail::json_quote(chosen.region) +
                 ",\"awslogs-stream-prefix\":\"cldmux\"}}}";
    const std::string definition_body =
        "{\"jobDefinitionName\":" + gcp::detail::json_quote(name) +
        ",\"type\":\"container\",\"platformCapabilities\":[\"" +
        (fargate ? "FARGATE" : "EC2") + "\"],"
        "\"containerProperties\":" +
        container + '}';
    const std::string endpoint = aws_batch_endpoint(*client, chosen.region);
    std::string definition;
    std::string registration_failure;
    try {
        const auto registered =
            gcp::detail::parse_json(aws_call(*client,
                                             gcp::detail::HttpRequest{}
                                                 .with_method("POST")
                                                 .with_url(endpoint + "/v1/registerjobdefinition")
                                                 .with_headers({"Content-Type: application/json"})
                                                 .with_body(definition_body),
                                             chosen.region, "batch", false)
                                        .body);
        definition = gcp::detail::field(registered, "jobDefinitionArn");
        if (definition.empty())
            registration_failure =
                "AWS Batch returned a successful response without jobDefinitionArn";
    } catch (const error& failure) {
        if (!retryable(failure.http_status()))
            throw;
        registration_failure = failure.what();
    }
    if (definition.empty()) {
        const auto recovery =
            std::min(client->config.cleanup_timeout, std::chrono::milliseconds(30'000));
        const auto deadline = std::chrono::steady_clock::now() + recovery;
        while (std::chrono::steady_clock::now() < deadline) {
            std::vector<std::string> found;
            try {
                found = find_aws_definitions(*client, endpoint, chosen.region, name, deadline);
            } catch (const error&) {
                pause(*client, deadline);
                continue;
            }
            if (found.size() == 1) {
                definition = std::move(found.front());
                break;
            }
            if (found.size() > 1) {
                bool cleanup_complete = true;
                for (const auto& arn : found)
                    try {
                        (void)aws_call(*client,
                                       gcp::detail::HttpRequest{}
                                           .with_method("POST")
                                           .with_url(endpoint + "/v1/deregisterjobdefinition")
                                           .with_headers({"Content-Type: application/json"})
                                           .with_body("{\"jobDefinition\":" +
                                                      gcp::detail::json_quote(arn) + "}"),
                                       chosen.region, "batch", false, deadline);
                    } catch (...) {
                        cleanup_complete = false;
                    }
                throw error(
                    "AWS Batch registration produced several active definitions for job "
                    "definition name " +
                    name +
                    (cleanup_complete ? "; all were deregistered" : "; cleanup was incomplete"));
            }
            pause(*client, deadline);
        }
    }
    if (definition.empty())
        throw error("AWS Batch registration outcome is unknown for job definition name " + name +
                    (registration_failure.empty() ? std::string{} : ": " + registration_failure));
    // Keep the historical cloud-hpp tag stable for external inventory policies.
    const std::string submit_body =
        "{\"jobName\":" + gcp::detail::json_quote(name) +
        ",\"jobQueue\":" + gcp::detail::json_quote(aws_queue(client->config, spec)) +
        ",\"jobDefinition\":" + gcp::detail::json_quote(definition) +
        ",\"retryStrategy\":{\"attempts\":" + std::to_string(spec.retries + 1) +
        "},\"timeout\":{\"attemptDurationSeconds\":" +
        std::to_string(provider_attempt_timeout_seconds(spec)) +
        "},\"tags\":{\"cloud-hpp\":\"temporary\"}}";
    const std::string queue = aws_queue(client->config, spec);
    std::string id;
    std::string ambiguous_failure;
    try {
        const auto submitted =
            gcp::detail::parse_json(aws_call(*client,
                                             gcp::detail::HttpRequest{}
                                                 .with_method("POST")
                                                 .with_url(endpoint + "/v1/submitjob")
                                                 .with_headers({"Content-Type: application/json"})
                                                 .with_body(submit_body),
                                             chosen.region, "batch", false)
                                        .body);
        id = gcp::detail::field(submitted, "jobId");
        if (id.empty())
            ambiguous_failure = "AWS Batch returned a successful response without jobId";
    } catch (const error& failure) {
        if (retryable(failure.http_status())) {
            ambiguous_failure = failure.what();
        } else {
            try {
                (void)aws_call(*client,
                               gcp::detail::HttpRequest{}
                                   .with_method("POST")
                                   .with_url(endpoint + "/v1/deregisterjobdefinition")
                                   .with_headers({"Content-Type: application/json"})
                                   .with_body("{\"jobDefinition\":" +
                                              gcp::detail::json_quote(definition) + "}"),
                               chosen.region, "batch", false);
            } catch (...) {
            }
            throw;
        }
    }
    if (id.empty()) {
        const auto recovery =
            std::min(client->config.cleanup_timeout, std::chrono::milliseconds(30'000));
        const auto deadline = std::chrono::steady_clock::now() + recovery;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                if (const auto found =
                        find_aws_job(*client, endpoint, chosen.region, queue, name, deadline)) {
                    id = *found;
                    break;
                }
            } catch (const error&) {
            }
            pause(*client, deadline);
        }
    }
    if (id.empty()) {
        // The randomised job name is an audit handle. Keep the definition active:
        // an accepted but not-yet-visible job can still depend on it.
        throw error("AWS Batch submission outcome is unknown for job name " + name +
                    (ambiguous_failure.empty() ? std::string{} : ": " + ambiguous_failure));
    }
    auto data = std::make_shared<job_data>();
    data->client = std::move(client);
    data->spec = spec;
    data->chosen = chosen;
    data->id = id;
    data->name = name;
    data->auxiliary = definition;
    return data;
}

inline std::string azure_command(const std::vector<std::string>& command) {
    std::string out;
    for (const auto& argument : command) {
        if (!out.empty())
            out.push_back(' ');
        out += argument;
    }
    return out;
}

inline std::shared_ptr<job_data> submit_azure(std::shared_ptr<client_state> client,
                                              const job_spec& spec, const cldmux::plan& chosen,
                                              const std::string& id) {
    // Create fixed-ID job and task resources around a one-node, job-lifetime
    // auto-pool. After conflicts or transport ambiguity, inspect the fixed resource
    // before repeating POST. Task-creation failure triggers best-effort job cleanup.
    const auto& azure = client->config.azure;
    const std::string endpoint = gcp::detail::base_url(azure.batch_endpoint);
    if (endpoint.empty())
        throw error("Azure jobs require config::azure.batch_endpoint");
    const bool spot = spec.resources.spot;
    const std::uint64_t watchdog_seconds = azure_job_timeout_seconds(spec, client->config);
    std::string pool =
        "{\"vmSize\":" + gcp::detail::json_quote(chosen.machine_type) +
        ",\"targetDedicatedNodes\":" + (spot ? "0" : "1") +
        ",\"targetLowPriorityNodes\":" + (spot ? "1" : "0") +
        ",\"taskSlotsPerNode\":1,\"virtualMachineConfiguration\":{"
        "\"imageReference\":{\"publisher\":" +
        gcp::detail::json_quote(azure.image_publisher) +
        ",\"offer\":" + gcp::detail::json_quote(azure.image_offer) +
        ",\"sku\":" + gcp::detail::json_quote(azure.image_sku) +
        ",\"version\":\"latest\"},\"nodeAgentSKUId\":" +
        gcp::detail::json_quote(azure.node_agent_sku) +
        ",\"containerConfiguration\":{\"type\":\"dockerCompatible\"}}";
    if (!spec.mounts.empty()) {
        std::string mounts;
        for (std::size_t i = 0; i < spec.mounts.size(); ++i) {
            const auto& item = spec.mounts[i];
            const uri source = parse_uri(item.source);
            if (!mounts.empty())
                mounts += ',';
            mounts += "{\"azureBlobFileSystemConfiguration\":{\"accountName\":" +
                      gcp::detail::json_quote(azure.storage_account) +
                      ",\"containerName\":" + gcp::detail::json_quote(source.bucket) +
                      ",\"relativeMountPath\":" +
                      gcp::detail::json_quote("cldmux-" + std::to_string(i)) +
                      ",\"sasKey\":" + gcp::detail::json_quote(azure.storage_sas);
            if (item.read_only)
                mounts += ",\"blobfuseOptions\":\"-o ro\"";
            mounts += "}}";
        }
        pool += ",\"mountConfiguration\":[" + mounts + ']';
    }
    pool += '}';
    const std::string job_body =
        "{\"id\":" + gcp::detail::json_quote(id) +
        ",\"onAllTasksComplete\":\"noaction\",\"constraints\":{\"maxWallClockTime\":" +
        gcp::detail::json_quote("PT" + std::to_string(watchdog_seconds) + "S") +
        "},\"poolInfo\":{\"autoPoolSpecification\":{\"autoPoolIdPrefix\":\"cldmux\","
        "\"poolLifetimeOption\":\"job\",\"keepAlive\":false,\"pool\":" +
        pool + "}}}";
    const auto create_fixed = [&](std::string collection_url, std::string resource_url,
                                  const std::string& body, std::string_view kind) {
        const auto recovery =
            std::min(client->config.cleanup_timeout, std::chrono::milliseconds(30'000));
        const auto deadline = std::chrono::steady_clock::now() + recovery;
        std::string last_failure;
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                (void)azure_call(*client,
                                 gcp::detail::HttpRequest{}
                                     .with_method("POST")
                                     .with_url(collection_url)
                                     .with_headers({"Content-Type: application/json"})
                                     .with_body(body),
                                 false, deadline);
                return;
            } catch (const error& failure) {
                if (failure.http_status() != 409 && !retryable(failure.http_status()))
                    throw;
                last_failure = failure.what();
            }
            try {
                (void)azure_call(*client, gcp::detail::HttpRequest{}.with_url(resource_url), true,
                                 deadline);
                return;
            } catch (const error& inspection) {
                if (inspection.http_status() != 404 && !retryable(inspection.http_status()))
                    throw;
            }
            if (attempt != 2)
                pause(*client, deadline);
        }
        throw error("Azure Batch " + std::string(kind) +
                    " creation outcome is unknown for fixed id " + id +
                    (last_failure.empty() ? std::string{} : ": " + last_failure));
    };
    create_fixed(azure_batch_url(*client, "/jobs"),
                 azure_batch_url(*client, "/jobs/" + gcp::detail::encode(id)), job_body, "job");

    const std::string task_id = "task";
    std::string run_options = "--rm";
    if (!spec.workdir.empty())
        run_options += " --workdir=" + spec.workdir;
    for (std::size_t i = 0; i < spec.mounts.size(); ++i) {
        const auto& item = spec.mounts[i];
        run_options += " --volume=/mnt/batch/tasks/fsmounts/cldmux-" + std::to_string(i) + ':' +
                       item.target + (item.read_only ? ":ro" : "");
    }
    const std::string mount_identity =
        spec.mounts.empty()
            ? std::string{}
            : ",\"userIdentity\":{\"autoUser\":{\"scope\":\"task\","
              "\"elevationLevel\":\"admin\"}}";
    const std::string task_body =
        "{\"id\":\"task\",\"commandLine\":" + gcp::detail::json_quote(azure_command(spec.command)) +
        ",\"constraints\":{\"maxWallClockTime\":" +
        gcp::detail::json_quote(
            "PT" + std::to_string(provider_attempt_timeout_seconds(spec)) + "S") +
        ",\"maxTaskRetryCount\":" + std::to_string(spec.retries) +
        ",\"retentionTime\":\"PT1H\"},\"containerSettings\":{\"imageName\":" +
        gcp::detail::json_quote(spec.image) +
        ",\"containerRunOptions\":" + gcp::detail::json_quote(run_options) + "}" +
        mount_identity + '}';
    try {
        create_fixed(azure_batch_url(*client, "/jobs/" + gcp::detail::encode(id) + "/tasks"),
                     azure_batch_url(*client, "/jobs/" + gcp::detail::encode(id) + "/tasks/" +
                                                  gcp::detail::encode(task_id)),
                     task_body, "task");
    } catch (...) {
        try {
            (void)azure_call(*client,
                             gcp::detail::HttpRequest{}.with_method("DELETE").with_url(
                                 azure_batch_url(*client, "/jobs/" + gcp::detail::encode(id))),
                             false);
        } catch (...) {
        }
        throw;
    }
    auto data = std::make_shared<job_data>();
    data->client = std::move(client);
    data->spec = spec;
    data->chosen = chosen;
    data->id = id;
    data->name = id;
    data->auxiliary = task_id;
    return data;
}

} // namespace detail
} // namespace cldmux
