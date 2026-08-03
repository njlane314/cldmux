#pragma once

#include "cloud/detail/pricing.hpp"

namespace cloud {
namespace detail {

// Job state, logs, cancellation, and cleanup ----------------------------------

inline void pause(const client_state& client, std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining > std::chrono::milliseconds::zero())
        std::this_thread::sleep_for(std::min(client.config.poll_interval, remaining));
}

struct job_data {
    // Every public job copy shares this mutable controller state. id is the
    // public provider ID, name is the provider lookup/audit name, and auxiliary
    // is the AWS definition ARN or Azure task ID (unused by GCP).
    std::shared_ptr<client_state> client;
    job_spec spec;
    cloud::plan chosen;
    std::string id;
    std::string name;
    std::string auxiliary;
    mutable std::string uid;
    mutable bool cancel_requested = false;
    mutable std::optional<result> cached;
    mutable std::vector<log_entry> log_cache;
    mutable std::unordered_set<std::string> log_ids;

    // GCP tracks a receive-time overlap/high-water pair; AWS tracks one token per
    // CloudWatch stream; Azure tracks epoch-keyed byte offsets and incomplete
    // line tails. These checkpoints are committed only after a successful poll.
    mutable std::string log_cursor;
    mutable std::string log_high_water;
    mutable std::map<std::string, std::string, std::less<>> log_cursors;
    mutable std::map<std::string, std::uint64_t, std::less<>> log_offsets;
    mutable std::map<std::string, std::string, std::less<>> log_tails;
    mutable std::map<std::string, std::uint64_t, std::less<>> log_tail_offsets;
    mutable std::string azure_log_epoch;
    mutable std::uint64_t azure_log_generation = 0;
    std::chrono::steady_clock::time_point submitted = std::chrono::steady_clock::now();
};

inline job_state parse_state(std::string_view state) {
    if (state == "QUEUED")
        return job_state::queued;
    if (state == "SCHEDULED")
        return job_state::scheduled;
    if (state == "RUNNING")
        return job_state::running;
    if (state == "SUCCEEDED")
        return job_state::succeeded;
    if (state == "FAILED")
        return job_state::failed;
    if (state == "CANCELLATION_IN_PROGRESS")
        return job_state::cancelling;
    if (state == "CANCELLED")
        return job_state::cancelled;
    if (state == "DELETION_IN_PROGRESS")
        return job_state::deleting;
    return job_state::unknown;
}

inline bool terminal(job_state state) {
    return state == job_state::succeeded || state == job_state::failed ||
           state == job_state::cancelled;
}

inline std::string aws_batch_endpoint(const client_state& client, std::string_view region) {
    return aws_endpoint(client, client.config.aws.batch_endpoint, "batch", region);
}

inline std::string aws_logs_endpoint(const client_state& client, std::string_view region) {
    return aws_endpoint(client, client.config.aws.logs_endpoint, "logs", region);
}

inline std::string azure_batch_url(const client_state& client, std::string path) {
    if (client.config.azure.batch_endpoint.empty())
        throw error("Azure jobs require config::azure.batch_endpoint");
    const char separator = path.find('?') == std::string::npos ? '?' : '&';
    return gcp::detail::base_url(client.config.azure.batch_endpoint) + std::move(path) + separator +
           "api-version=" + gcp::detail::encode(client.config.azure.api_version);
}

inline const gcp::detail::Json* aws_job_object(const gcp::detail::Json& json) {
    const auto* jobs = json.get("jobs");
    return jobs && !jobs->array().empty() ? &jobs->array().front() : nullptr;
}

inline gcp::detail::Json get_job(
    const job_data& job,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    if (job.chosen.provider == "gcp") {
        auto response = call(*job.client,
                             gcp::detail::HttpRequest{}.with_url(
                                 job.client->core()->config.batch_endpoint + "/v1/" + job.name),
                             deadline);
        return gcp::detail::parse_json(response.body);
    }
    if (job.chosen.provider == "aws") {
        const auto response = aws_call(
            *job.client,
            gcp::detail::HttpRequest{}
                .with_method("POST")
                .with_url(aws_batch_endpoint(*job.client, job.chosen.region) + "/v1/describejobs")
                .with_headers({"Content-Type: application/json"})
                .with_body("{\"jobs\":[" + gcp::detail::json_quote(job.id) + "]}"),
            job.chosen.region, "batch", true, deadline);
        const auto outer = gcp::detail::parse_json(response.body);
        const auto* value = aws_job_object(outer);
        if (!value)
            throw error("AWS Batch no longer returned job " + job.id);
        return *value;
    }
    const auto response =
        azure_call(*job.client,
                   gcp::detail::HttpRequest{}.with_url(azure_batch_url(
                       *job.client, "/jobs/" + gcp::detail::encode(job.name) + "/tasks/" +
                                        gcp::detail::encode(job.auxiliary))),
                   true, deadline);
    return gcp::detail::parse_json(response.body);
}

inline job_state update(const job_data& job, const gcp::detail::Json& json) {
    if (job.chosen.provider == "gcp") {
        if (job.uid.empty())
            job.uid = gcp::detail::field(json, "uid");
        const auto* status = json.get("status");
        return status ? parse_state(gcp::detail::field(*status, "state")) : job_state::unknown;
    }
    if (job.chosen.provider == "aws") {
        const std::string state = gcp::detail::field(json, "status");
        if (state == "SUBMITTED" || state == "PENDING")
            return job_state::queued;
        if (state == "RUNNABLE" || state == "STARTING")
            return job_state::scheduled;
        if (state == "RUNNING")
            return job_state::running;
        if (state == "SUCCEEDED")
            return job_state::succeeded;
        if (state == "FAILED") {
            const std::string reason = gcp::detail::field(json, "statusReason");
            return contains(reason, "cloud cancellation") ? job_state::cancelled
                                                            : job_state::failed;
        }
        return job_state::unknown;
    }
    const std::string state = lowercase(gcp::detail::field(json, "state"));
    if (state == "active")
        return job_state::queued;
    if (state == "preparing")
        return job_state::scheduled;
    if (state == "running")
        return job_state::running;
    if (state == "completed") {
        const auto* execution = json.get("executionInfo");
        if (execution && lowercase(gcp::detail::field(*execution, "result")) == "success")
            return job_state::succeeded;
        return job.cancel_requested ? job_state::cancelled : job_state::failed;
    }
    return job_state::unknown;
}

inline std::string status_error(const job_data& job, const gcp::detail::Json& json) {
    if (job.chosen.provider == "aws") {
        std::string message = gcp::detail::field(json, "statusReason");
        if (const auto* container = json.get("container")) {
            const std::string reason = gcp::detail::field(*container, "reason");
            if (!reason.empty())
                message = reason;
        }
        return message.empty() ? "AWS Batch job failed" : message;
    }
    if (job.chosen.provider == "azure") {
        const auto* execution = json.get("executionInfo");
        const auto* failure = execution ? execution->get("failureInfo") : nullptr;
        std::string message = failure ? gcp::detail::field(*failure, "message") : std::string{};
        return message.empty() ? "Azure Batch task failed" : message;
    }
    const auto* status = json.get("status");
    std::string message;
    if (status)
        gcp::detail::for_each_json(*status, "statusEvents", [&](const gcp::detail::Json& event) {
            const std::string value = gcp::detail::field(event, "description");
            if (!value.empty())
                message = value;
        });
    return message.empty() ? "Batch job failed" : message;
}

inline std::optional<int> task_exit_code(const job_data& job) {
    if (job.chosen.provider != "gcp") {
        try {
            const auto json = get_job(job);
            const auto* source =
                job.chosen.provider == "aws" ? json.get("container") : json.get("executionInfo");
            const std::string value =
                source ? gcp::detail::field(*source, "exitCode") : std::string{};
            if (value.empty())
                return std::nullopt;
            return std::stoi(value);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    try {
        const auto deadline = std::chrono::steady_clock::now() + job.client->config.request_timeout;
        auto response = call(
            *job.client,
            gcp::detail::HttpRequest{}.with_url(job.client->core()->config.batch_endpoint + "/v1/" +
                                                job.name + "/taskGroups/group0/tasks/0"),
            deadline);
        const auto json = gcp::detail::parse_json(response.body);
        const auto* status = json.get("status");
        const auto* events = status ? status->get("statusEvents") : nullptr;
        std::optional<int> code;
        if (events)
            for (const auto& event : events->array())
                if (const auto* execution = event.get("taskExecution")) {
                    const std::string value = gcp::detail::field(*execution, "exitCode");
                    if (!value.empty()) {
                        try {
                            code = std::stoi(value);
                        } catch (const std::exception&) {
                            return std::nullopt;
                        }
                    }
                }
        return code;
    } catch (const error&) {
        return std::nullopt;
    }
}

inline log_entry make_log_entry(std::string timestamp, std::string receive_timestamp,
                                std::string id, std::string text, std::string severity) {
    log_entry entry;
    entry.timestamp = std::move(timestamp);
    entry.receive_timestamp = std::move(receive_timestamp);
    entry.id = std::move(id);
    entry.text = std::move(text);
    entry.severity = std::move(severity);
    return entry;
}

inline std::vector<log_entry> logs(
    const job_data& job,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    if (job.chosen.provider == "aws") {
        // A retry can create one CloudWatch stream per attempt. Work on copied
        // cursors and commit them only after every stream converges so a partial
        // polling failure remains retryable without gaps.
        const auto current = get_job(job, deadline);
        std::vector<std::string> streams;
        const auto add_stream = [&](const gcp::detail::Json& value) {
            const auto* container = value.get("container");
            const std::string stream =
                container ? gcp::detail::field(*container, "logStreamName") : std::string{};
            if (!stream.empty() &&
                std::find(streams.begin(), streams.end(), stream) == streams.end())
                streams.push_back(stream);
        };
        gcp::detail::for_each_json(current, "attempts", add_stream);
        add_stream(current);
        std::vector<log_entry> out;
        auto next_cursors = job.log_cursors;
        for (const auto& stream : streams) {
            std::string token = next_cursors[stream];
            bool converged = false;
            for (int page = 0; page < 100; ++page) {
                std::string body = "{\"logGroupName\":" +
                                   gcp::detail::json_quote(job.client->config.aws.log_group) +
                                   ",\"logStreamName\":" + gcp::detail::json_quote(stream) +
                                   ",\"startFromHead\":true";
                if (!token.empty())
                    body += ",\"nextToken\":" + gcp::detail::json_quote(token);
                body += '}';
                const auto response =
                    aws_call(*job.client,
                             gcp::detail::HttpRequest{}
                                 .with_method("POST")
                                 .with_url(aws_logs_endpoint(*job.client, job.chosen.region) + '/')
                                 .with_headers({"Content-Type: application/x-amz-json-1.1",
                                                "X-Amz-Target: Logs_20140328.GetLogEvents"})
                                 .with_body(std::move(body)),
                             job.chosen.region, "logs", true, deadline);
                const auto json = gcp::detail::parse_json(response.body);
                std::size_t index = 0;
                gcp::detail::for_each_json(json, "events", [&](const gcp::detail::Json& event) {
                    const std::string timestamp = gcp::detail::field(event, "timestamp");
                    const std::string received = gcp::detail::field(event, "ingestionTime");
                    const std::string text = gcp::detail::field(event, "message");
                    out.push_back(make_log_entry(timestamp, received,
                                                 stream + ':' + token + ':' + timestamp + ':' +
                                                     received + ':' + std::to_string(index++),
                                                 text, "DEFAULT"));
                });
                const std::string next = gcp::detail::field(json, "nextForwardToken");
                if (next.empty() || next == token) {
                    converged = true;
                    break;
                }
                token = next;
                next_cursors[stream] = token;
            }
            if (!converged)
                throw error("AWS CloudWatch Logs pagination did not converge within 100 pages");
        }
        job.log_cursors = std::move(next_cursors);
        return out;
    }
    if (job.chosen.provider == "azure") {
        // Azure exposes append-only stdout/stderr files rather than log events.
        // An execution epoch distinguishes retries/requeues; range offsets resume
        // reads and tails retain incomplete lines until newline or terminal state.
        const auto task = get_job(job, deadline);
        const bool final = terminal(update(job, task));
        const auto* execution = task.get("executionInfo");
        const auto execution_field = [&](std::string_view name, std::string fallback = {}) {
            if (!execution)
                return fallback;
            std::string value = gcp::detail::field(*execution, name);
            return value.empty() ? fallback : value;
        };
        const std::string start_time = execution_field("startTime");
        const std::string retry_count = execution_field("retryCount", "0");
        const std::string requeue_count = execution_field("requeueCount", "0");
        const std::string epoch = std::to_string(start_time.size()) + ':' + start_time + ':' +
                                  retry_count + ':' + requeue_count;
        std::vector<log_entry> out;
        auto offsets = job.log_offsets;
        auto tails = job.log_tails;
        auto tail_offsets = job.log_tail_offsets;
        std::string active_epoch = job.azure_log_epoch;
        std::uint64_t active_generation = job.azure_log_generation;
        const auto order = [](std::uint64_t generation, std::string_view stream,
                              std::uint64_t offset) {
            std::ostringstream value;
            value << std::setw(20) << std::setfill('0') << generation << ':'
                  << (stream == "stdout.txt" ? '0' : '1') << ':' << std::setw(20)
                  << std::setfill('0') << offset;
            return value.str();
        };
        if (active_epoch.empty()) {
            active_epoch = epoch;
        } else if (active_epoch != epoch) {
            for (const std::string_view stream :
                 {std::string_view("stdout.txt"), std::string_view("stderr.txt")}) {
                const std::string old_key = active_epoch + ':' + std::string(stream);
                auto tail = tails.find(old_key);
                if (tail == tails.end() || tail->second.empty())
                    continue;
                std::string text = std::move(tail->second);
                tail->second.clear();
                if (!text.empty() && text.back() == '\r')
                    text.pop_back();
                const std::uint64_t offset = tail_offsets[old_key];
                out.push_back(make_log_entry(order(active_generation, stream, offset), {},
                                             old_key + ':' + std::to_string(offset),
                                             std::move(text),
                                             stream == "stderr.txt" ? "ERROR" : "DEFAULT"));
                tail_offsets[old_key] = offsets[old_key];
            }
            active_epoch = epoch;
            ++active_generation;
        }
        for (const std::string_view stream :
             {std::string_view("stdout.txt"), std::string_view("stderr.txt")}) {
            const std::string key = epoch + ':' + std::string(stream);
            const std::string path = "/jobs/" + gcp::detail::encode(job.name) + "/tasks/" +
                                     gcp::detail::encode(job.auxiliary) + "/files/" +
                                     gcp::detail::encode(stream);
            gcp::detail::HttpResponse properties;
            try {
                properties = azure_call(*job.client,
                                        gcp::detail::HttpRequest{}
                                            .with_method("HEAD")
                                            .with_url(azure_batch_url(*job.client, path))
                                            .with_accept_json(false),
                                        true, deadline);
            } catch (const error& failure) {
                if (failure.http_status() == 404)
                    continue;
                throw;
            }
            const auto length_header = properties.headers.find("content-length");
            if (length_header == properties.headers.end())
                throw error("Azure Batch task-file response omitted Content-Length");
            std::uint64_t length = 0;
            try {
                std::size_t used = 0;
                length = std::stoull(length_header->second, &used);
                if (used != length_header->second.size())
                    throw std::invalid_argument("suffix");
            } catch (const std::exception&) {
                throw error("Azure Batch task-file response had invalid Content-Length");
            }
            if (length < offsets[key]) {
                offsets[key] = 0;
                tails[key].clear();
                tail_offsets[key] = 0;
            }
            const std::uint64_t start = offsets[key];
            std::string bytes;
            if (length > start) {
                constexpr std::uint64_t chunk_bytes = 16 * 1024 * 1024;
                const std::uint64_t end = start + std::min(chunk_bytes, length - start) - 1;
                const auto response =
                    azure_call(*job.client,
                               gcp::detail::HttpRequest{}
                                   .with_url(azure_batch_url(*job.client, path))
                                   .with_headers({gcp::detail::header(
                                       "ocp-range", "bytes=" + std::to_string(start) + '-' +
                                                        std::to_string(end))})
                                   .with_accept_json(false),
                               true, deadline);
                bytes = response.body;
                const auto expected = end - start + 1;
                if (bytes.size() != expected) {
                    if (response.status == 200 && bytes.size() == length && start <= bytes.size())
                        bytes.erase(0, static_cast<std::size_t>(start));
                    else
                        throw error(
                            "Azure Batch task-file range returned an unexpected byte count");
                }
                offsets[key] = end + 1;
            }
            std::uint64_t base = start;
            if (!tails[key].empty()) {
                constexpr std::size_t max_log_line_bytes = 16 * 1024 * 1024;
                const auto newline = bytes.find('\n');
                const std::size_t continuation =
                    newline == std::string::npos ? bytes.size() : newline;
                if (continuation > max_log_line_bytes ||
                    tails[key].size() > max_log_line_bytes - continuation)
                    throw error("Azure Batch log line exceeded the private byte limit");
                base = tail_offsets[key];
                bytes.insert(0, tails[key]);
            }
            const bool stream_final = final && offsets[key] == length;
            std::size_t position = 0;
            while (position < bytes.size()) {
                const auto end = bytes.find('\n', position);
                if (end == std::string::npos && !stream_final)
                    break;
                const auto line_length = (end == std::string::npos ? bytes.size() : end) - position;
                std::string text = bytes.substr(position, line_length);
                if (!text.empty() && text.back() == '\r')
                    text.pop_back();
                out.push_back(make_log_entry(order(active_generation, stream, base + position), {},
                                             key + ':' + std::to_string(base + position),
                                             std::move(text),
                                             stream == "stderr.txt" ? "ERROR" : "DEFAULT"));
                if (end == std::string::npos)
                    break;
                position = end + 1;
            }
            if (position < bytes.size() && !stream_final) {
                constexpr std::size_t max_log_line_bytes = 16 * 1024 * 1024;
                if (bytes.size() - position > max_log_line_bytes)
                    throw error("Azure Batch log line exceeded the private byte limit");
                tails[key] = bytes.substr(position);
                tail_offsets[key] = base + position;
            } else {
                tails[key].clear();
                tail_offsets[key] = offsets[key];
            }
        }
        job.log_offsets = std::move(offsets);
        job.log_tails = std::move(tails);
        job.log_tail_offsets = std::move(tail_offsets);
        job.azure_log_epoch = std::move(active_epoch);
        job.azure_log_generation = active_generation;
        return out;
    }
    if (job.uid.empty())
        update(job, get_job(job, deadline));
    std::vector<log_entry> out;
    std::string page;
    // Query from the previous high-water receive time, then deduplicate by
    // stable log identity. The overlap avoids gaps when Logging exposes entries
    // with equal or delayed timestamps on different polls.
    do {
        const std::string project = job.client->core()->project();
        validate_project(project);
        const std::string filter =
            "logName = \"projects/" + project +
            "/logs/batch_task_logs\" AND labels.job_uid=" + job.uid +
            (job.log_cursor.empty() ? std::string{}
                                    : " AND receiveTimestamp >= \"" + job.log_cursor + "\"");
        std::string body = "{\"resourceNames\":[" + gcp::detail::json_quote("projects/" + project) +
                           "],\"filter\":" + gcp::detail::json_quote(filter) +
                           ",\"orderBy\":\"timestamp desc\"";
        if (!page.empty())
            body += ",\"pageToken\":" + gcp::detail::json_quote(page);
        body += '}';
        auto response =
            call(*job.client,
                 gcp::detail::HttpRequest{}
                     .with_method("POST")
                     .with_url(job.client->core()->config.logging_endpoint + "/v2/entries:list")
                     .with_headers({"Content-Type: application/json"})
                     .with_body(std::move(body)),
                 deadline);
        const auto json = gcp::detail::parse_json(response.body);
        gcp::detail::for_each_json(json, "entries", [&](const gcp::detail::Json& entry) {
            log_entry line = make_log_entry(gcp::detail::field(entry, "timestamp"),
                                            gcp::detail::field(entry, "receiveTimestamp"),
                                            gcp::detail::field(entry, "insertId"),
                                            gcp::detail::field(entry, "textPayload"),
                                            gcp::detail::field(entry, "severity"));
            if (line.text.empty())
                if (const auto* payload = entry.get("jsonPayload"))
                    line.text = gcp::detail::field(*payload, "message");
            if (line.text.empty())
                line.text = "[structured log entry]";
            if (line.id.empty())
                line.id = line.timestamp + '\n' + line.text;
            out.push_back(std::move(line));
        });
        page = gcp::detail::field(json, "nextPageToken");
    } while (!page.empty());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.timestamp, a.id) < std::tie(b.timestamp, b.id);
    });
    for (const auto& line : out) {
        std::string received = line.receive_timestamp;
        if (received.size() == 20 && received.back() == 'Z')
            received.insert(19, ".000000000");
        else if (received.size() > 21 && received[19] == '.' && received.back() == 'Z' &&
                 received.size() <= 29 &&
                 std::all_of(received.begin() + 20, received.end() - 1,
                             [](char c) { return c >= '0' && c <= '9'; }))
            received.insert(received.size() - 1, 9 - (received.size() - 21), '0');
        if (received.empty())
            continue;
        if (job.log_high_water.empty() || received > job.log_high_water) {
            job.log_cursor = job.log_high_water;
            job.log_high_water = std::move(received);
        } else if (received < job.log_high_water &&
                   (job.log_cursor.empty() || received > job.log_cursor)) {
            job.log_cursor = std::move(received);
        }
    }
    return out;
}

inline std::vector<log_entry> merge_logs(
    const job_data& job,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    // All provider readers may overlap prior data. This single identity set makes
    // repeated status/log polling idempotent and retains a stable cached snapshot.
    std::vector<log_entry> added;
    for (auto& line : logs(job, deadline)) {
        const std::string identity = line.timestamp + '\n' + line.id;
        if (job.log_ids.insert(identity).second) {
            added.push_back(line);
            job.log_cache.push_back(std::move(line));
        }
    }
    std::sort(job.log_cache.begin(), job.log_cache.end(), [](const auto& a, const auto& b) {
        return std::tie(a.timestamp, a.id) < std::tie(b.timestamp, b.id);
    });
    return added;
}

template <typename Poll> inline void drain_logs(const job_data& job, Poll poll) {
    // Terminal state can become visible before final log entries. Stop after a
    // configurable quiet period, but always cap the best-effort drain duration.
    const auto stop = std::chrono::steady_clock::now() + job.client->config.final_log_timeout;
    auto quiet_since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < stop) {
        if (poll(stop))
            quiet_since = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - quiet_since >= job.client->config.final_log_delay)
            break;
        pause(*job.client, stop);
    }
}

inline void wait_operation(const client_state& client, const std::string& name,
                           std::chrono::steady_clock::time_point deadline) {
    if (name.empty())
        throw error("Malformed Batch operation: missing name");
    while (std::chrono::steady_clock::now() < deadline) {
        auto response = call(client,
                             gcp::detail::HttpRequest{}.with_url(
                                 client.core()->config.batch_endpoint + "/v1/" + name),
                             deadline);
        const auto json = gcp::detail::parse_json(response.body);
        if (const auto* done = json.get("done"); done && done->boolean()) {
            if (const auto* failure = json.get("error")) {
                std::string message = gcp::detail::field(*failure, "message");
                throw error("Batch cleanup failed" +
                            (message.empty() ? std::string{} : ": " + message));
            }
            return;
        }
        pause(client, deadline);
    }
    throw error("Timed out waiting for Batch operation");
}

inline void delete_job(const job_data& job, std::string_view reason) {
    // "Delete" is provider-specific cleanup: AWS can only deregister the
    // temporary definition; Azure must terminate the job to release its lifetime
    // auto-pool and deletes the record only when requested; GCP deletes the job
    // and waits for the returned long-running operation.
    const auto deadline = std::chrono::steady_clock::now() + job.client->config.cleanup_timeout;
    if (job.chosen.provider == "aws") {
        if (job.auxiliary.empty())
            return;
        (void)aws_call(
            *job.client,
            gcp::detail::HttpRequest{}
                .with_method("POST")
                .with_url(aws_batch_endpoint(*job.client, job.chosen.region) +
                          "/v1/deregisterjobdefinition")
                .with_headers({"Content-Type: application/json"})
                .with_body("{\"jobDefinition\":" + gcp::detail::json_quote(job.auxiliary) + "}"),
            job.chosen.region, "batch", false, deadline);
        return;
    }
    if (job.chosen.provider == "azure") {
        bool termination_uncertain = false;
        const auto request_termination = [&] {
            try {
                (void)azure_call(
                    *job.client,
                    gcp::detail::HttpRequest{}
                        .with_method("POST")
                        .with_url(azure_batch_url(
                            *job.client, "/jobs/" + gcp::detail::encode(job.name) + "/terminate"))
                        .with_headers({"Content-Type: application/json"})
                        .with_body("{\"terminateReason\":" + gcp::detail::json_quote(reason) + "}"),
                    false, deadline);
                termination_uncertain = false;
                return true;
            } catch (const error& failure) {
                if (failure.http_status() == 404)
                    return false;
                if (failure.http_status() == 409) {
                    termination_uncertain = false;
                    return true;
                }
                if (retryable(failure.http_status())) {
                    termination_uncertain = true;
                    return true;
                }
                throw;
            }
        };
        if (!request_termination())
            return;
        bool completed = false;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                const auto response =
                    azure_call(*job.client,
                               gcp::detail::HttpRequest{}.with_url(azure_batch_url(
                                   *job.client, "/jobs/" + gcp::detail::encode(job.name))),
                               true, deadline);
                const auto json = gcp::detail::parse_json(response.body);
                if (lowercase(gcp::detail::field(json, "state")) == "completed") {
                    completed = true;
                    break;
                }
            } catch (const error& failure) {
                if (failure.http_status() == 404)
                    return;
                throw;
            }
            if (termination_uncertain && !request_termination())
                return;
            pause(*job.client, deadline);
        }
        if (!completed)
            throw error("Timed out waiting for Azure Batch job termination");
        if (!job.spec.auto_delete)
            return;
        const auto request_delete = [&] {
            try {
                (void)azure_call(
                    *job.client,
                    gcp::detail::HttpRequest{}.with_method("DELETE").with_url(
                        azure_batch_url(*job.client, "/jobs/" + gcp::detail::encode(job.name))),
                    false, deadline);
                return true;
            } catch (const error& failure) {
                if (failure.http_status() == 404)
                    return false;
                if (failure.http_status() == 409 || retryable(failure.http_status()))
                    return true;
                throw;
            }
        };
        if (!request_delete())
            return;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                const auto response =
                    azure_call(*job.client,
                               gcp::detail::HttpRequest{}.with_url(azure_batch_url(
                                   *job.client, "/jobs/" + gcp::detail::encode(job.name))),
                               true, deadline);
                const auto state =
                    lowercase(gcp::detail::field(gcp::detail::parse_json(response.body), "state"));
                // A status-less DELETE can fail before reaching Azure. Once existence
                // is confirmed, safely repeat the fixed-resource deletion request.
                if (state != "deleting" && !request_delete())
                    return;
            } catch (const error& failure) {
                if (failure.http_status() == 404)
                    return;
                throw;
            }
            pause(*job.client, deadline);
        }
        throw error("Timed out waiting for Azure Batch job deletion");
    }
    gcp::detail::HttpResponse response;
    try {
        response = call(*job.client,
                        gcp::detail::HttpRequest{}.with_method("DELETE").with_url(
                            job.client->core()->config.batch_endpoint + "/v1/" + job.name +
                            "?reason=" + gcp::detail::encode(reason) +
                            "&requestId=" + gcp::detail::random_uuid()),
                        deadline);
    } catch (const error& failure) {
        if (failure.http_status() == 404)
            return;
        throw;
    }
    wait_operation(*job.client, gcp::detail::field(gcp::detail::parse_json(response.body), "name"),
                   deadline);
}

inline gcp::detail::Json wait_terminal(const job_data& job,
                                       std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        auto json = get_job(job, deadline);
        if (terminal(update(job, json)))
            return json;
        pause(*job.client, deadline);
    }
    throw error("Timed out waiting for Batch job to become terminal");
}

inline gcp::detail::Json cancel_job(const job_data& job) {
    // Cancellation first observes current state. AWS escalates CancelJob to
    // TerminateJob after a job starts; Azure terminates the task; GCP sends an
    // idempotent cancel request. Ambiguous mutations are repeated only after a
    // subsequent read confirms that the resource is still live.
    const auto deadline = std::chrono::steady_clock::now() + job.client->config.cleanup_timeout;
    auto current = get_job(job, deadline);
    if (terminal(update(job, current)))
        return current;
    if (job.chosen.provider == "aws") {
        const std::string status = gcp::detail::field(current, "status");
        std::string action = status == "SUBMITTED" || status == "PENDING" || status == "RUNNABLE"
                                 ? "canceljob"
                                 : "terminatejob";
        bool mutation_uncertain = false;
        const auto request = [&](std::string_view operation) {
            try {
                (void)aws_call(*job.client,
                               gcp::detail::HttpRequest{}
                                   .with_method("POST")
                                   .with_url(aws_batch_endpoint(*job.client, job.chosen.region) +
                                             "/v1/" + std::string(operation))
                                   .with_headers({"Content-Type: application/json"})
                                   .with_body("{\"jobId\":" + gcp::detail::json_quote(job.id) +
                                              ",\"reason\":\"cloud cancellation\"}"),
                               job.chosen.region, "batch", false, deadline);
                mutation_uncertain = false;
            } catch (const error& failure) {
                if (!retryable(failure.http_status()))
                    throw;
                mutation_uncertain = true;
            }
            job.cancel_requested = true;
        };
        request(action);
        while (std::chrono::steady_clock::now() < deadline) {
            current = get_job(job, deadline);
            if (terminal(update(job, current)))
                return current;
            const std::string observed = gcp::detail::field(current, "status");
            if (action != "terminatejob" && (observed == "STARTING" || observed == "RUNNING")) {
                action = "terminatejob";
                mutation_uncertain = true;
            }
            if (mutation_uncertain)
                request(action);
            pause(*job.client, deadline);
        }
        throw error("Timed out waiting for AWS Batch cancellation");
    }
    if (job.chosen.provider == "azure") {
        bool termination_uncertain = false;
        const auto request_termination = [&] {
            try {
                (void)azure_call(
                    *job.client,
                    gcp::detail::HttpRequest{}
                        .with_method("POST")
                        .with_url(azure_batch_url(
                            *job.client, "/jobs/" + gcp::detail::encode(job.name) + "/tasks/" +
                                             gcp::detail::encode(job.auxiliary) + "/terminate"))
                        .with_headers({"Content-Type: application/json"})
                        .with_body("{}"),
                    false, deadline);
                termination_uncertain = false;
            } catch (const error& failure) {
                if (failure.http_status() == 409) {
                    termination_uncertain = false;
                } else if (retryable(failure.http_status())) {
                    termination_uncertain = true;
                } else {
                    throw;
                }
            }
            job.cancel_requested = true;
        };
        request_termination();
        while (std::chrono::steady_clock::now() < deadline) {
            current = get_job(job, deadline);
            if (terminal(update(job, current)))
                return current;
            if (termination_uncertain)
                request_termination();
            pause(*job.client, deadline);
        }
        throw error("Timed out waiting for Azure Batch cancellation");
    }
    const std::string request_id = gcp::detail::random_uuid();
    gcp::detail::HttpResponse response;
    try {
        response = call(
            *job.client,
            gcp::detail::HttpRequest{}
                .with_method("POST")
                .with_url(job.client->core()->config.batch_endpoint + "/v1/" + job.name + ":cancel")
                .with_headers({"Content-Type: application/json"})
                .with_body("{\"requestId\":" + gcp::detail::json_quote(request_id) + "}"),
            deadline);
    } catch (const error& failure) {
        current = get_job(job, deadline);
        const job_state state = update(job, current);
        if (terminal(state))
            return current;
        if (state == job_state::cancelling || retryable(failure.http_status()))
            return wait_terminal(job, deadline);
        throw;
    }
    wait_operation(*job.client, gcp::detail::field(gcp::detail::parse_json(response.body), "name"),
                   deadline);
    return wait_terminal(job, deadline);
}

inline result make_result(const job_data& job, const gcp::detail::Json& json) {
    const job_state state = update(job, json);
    result out;
    out.state = state;
    out.warnings = job.chosen.warnings;
    if (state == job_state::succeeded) {
        out.exit_code = 0;
    } else if (state == job_state::failed) {
        out.exit_code = task_exit_code(job);
        out.message = status_error(job, json);
    } else if (state == job_state::cancelled) {
        out.message = "Batch job was cancelled";
    }
    return out;
}

} // namespace detail
} // namespace cloud
