#pragma once

#include "cldmux/detail/providers/aws.hpp"
#include "cldmux/detail/providers/azure.hpp"
#include "cldmux/detail/storage.hpp"
#include "cldmux/detail/submission.hpp"

namespace cldmux {

// Public storage, compute, job, and client handles -----------------------------

// Route-bound object-storage facade. cloud://bucket/key names a GCS object, S3
// object, or Azure Blob according to the client's fixed provider.
class storage {
public:
    object put(std::string_view destination, std::string_view bytes,
               put_options options = {}) const {
        const auto uri = detail::parse_uri(destination, "put");
        if (provider() == "aws")
            return detail::aws_storage_put(*state_, uri, bytes, std::move(options));
        if (provider() == "azure")
            return detail::azure_storage_put(*state_, uri, bytes, std::move(options));
        return raw().bucket(uri.bucket).put(uri.key, bytes, std::move(options));
    }

    object put_file(std::string_view destination, const std::filesystem::path& source,
                    put_options options = {}) const {
        const auto uri = detail::parse_uri(destination, "put_file");
        if (provider() == "aws")
            return detail::aws_storage_put_file(*state_, uri, source, std::move(options));
        if (provider() == "azure")
            return detail::azure_storage_put_file(*state_, uri, source, std::move(options));
        return raw().bucket(uri.bucket).put_file(uri.key, source, std::move(options));
    }

    [[nodiscard]] std::string get(std::string_view source) const {
        const auto uri = detail::parse_uri(source, "get");
        if (provider() == "aws")
            return detail::aws_storage_get(*state_, uri);
        if (provider() == "azure")
            return detail::azure_storage_get(*state_, uri);
        return raw().bucket(uri.bucket).get(uri.key);
    }

    void get_file(std::string_view source, const std::filesystem::path& destination) const {
        const auto uri = detail::parse_uri(source, "get_file");
        if (provider() == "aws") {
            detail::aws_storage_get_file(*state_, uri, destination);
            return;
        }
        if (provider() == "azure") {
            detail::azure_storage_get_file(*state_, uri, destination);
            return;
        }
        raw().bucket(uri.bucket).get_file(uri.key, destination);
    }

    [[nodiscard]] object_list list(std::string_view source, list_options options = {}) const {
        const auto uri = detail::parse_uri(source);
        options.prefix = uri.key + options.prefix;
        if (provider() == "aws")
            return detail::aws_storage_list(*state_, uri, std::move(options));
        if (provider() == "azure")
            return detail::azure_storage_list(*state_, uri, std::move(options));
        return raw().bucket(uri.bucket).list(std::move(options));
    }

    [[nodiscard]] object stat(std::string_view source) const {
        const auto uri = detail::parse_uri(source, "stat");
        if (provider() == "aws")
            return detail::aws_storage_stat(*state_, uri);
        if (provider() == "azure")
            return detail::azure_storage_stat(*state_, uri);
        return raw().bucket(uri.bucket).stat(uri.key);
    }

    void remove(std::string_view source) const {
        const auto uri = detail::parse_uri(source, "remove");
        if (provider() == "aws") {
            detail::aws_storage_remove(*state_, uri);
            return;
        }
        if (provider() == "azure") {
            detail::azure_storage_remove(*state_, uri);
            return;
        }
        raw().bucket(uri.bucket).erase(uri.key);
    }

private:
    friend class client;
    explicit storage(std::shared_ptr<detail::client_state> state, cldmux::provider provider)
        : state_(std::move(state)), provider_(std::move(provider)) {}
    [[nodiscard]] const cldmux::provider& provider() const {
        return provider_;
    }
    [[nodiscard]] gcp::Cloud& raw() const {
        if (provider() != "gcp")
            throw error("Unsupported object-storage provider");
        return state_->raw;
    }
    std::shared_ptr<detail::client_state> state_;
    cldmux::provider provider_;
};

// Route-bound raw-instance escape hatch. Batch jobs do not use this path.
class compute {
public:
    [[nodiscard]] std::vector<instance> instances(std::size_t limit = 0) const {
        if (provider() == "aws")
            return detail::aws_compute_instances(*state_, limit);
        if (provider() == "azure")
            return detail::azure_compute_instances(*state_, limit);
        return raw().vms(limit);
    }
    [[nodiscard]] operation create(std::string name, std::string logical_template) const {
        if (provider() == "aws") {
            const auto native = detail::aws_compute_create(*state_, name, logical_template);
            return operation(native.id, native.zone,
                             [state = state_, id = native.id](auto timeout, auto poll) {
                detail::wait_aws_instance(*state, id, "running", timeout, poll);
            });
        }
        if (provider() == "azure") {
            auto native = detail::azure_compute_create(*state_, name, logical_template);
            const std::string operation_name = native.name;
            const std::string operation_location = native.location;
            return operation(operation_name, operation_location,
                             [state = state_, native = std::move(native)](auto timeout, auto poll) {
                                 detail::wait_azure_compute(*state, native, timeout, poll);
                             });
        }
        const auto configured = state_->config.instance_templates.find(logical_template);
        if (configured == state_->config.instance_templates.end() ||
            configured->second.gcp_instance_template.empty())
            throw error("Unknown GCP logical compute template: " + logical_template);
        return wrap(raw().create_from_template(std::move(name),
                                               configured->second.gcp_instance_template));
    }
    [[nodiscard]] operation start(std::string name) const {
        if (provider() == "aws") {
            const auto native = detail::aws_resolve_instance(*state_, name);
            detail::aws_compute_action(*state_, "StartInstances", native.id);
            return operation(native.id, native.zone,
                             [state = state_, id = native.id](auto timeout, auto poll) {
                                 detail::wait_aws_instance(*state, id, "running", timeout, poll);
                             });
        }
        if (provider() == "azure") {
            auto native = detail::azure_compute_action(*state_, name, "start", "running");
            const std::string operation_name = native.name;
            const std::string operation_location = native.location;
            return operation(operation_name, operation_location,
                             [state = state_, native = std::move(native)](auto timeout, auto poll) {
                                 detail::wait_azure_compute(*state, native, timeout, poll);
                             });
        }
        return wrap(raw().vm(std::move(name)).start());
    }
    [[nodiscard]] operation stop(std::string name) const {
        if (provider() == "aws") {
            const auto native = detail::aws_resolve_instance(*state_, name);
            detail::aws_compute_action(*state_, "StopInstances", native.id);
            return operation(native.id, native.zone,
                             [state = state_, id = native.id](auto timeout, auto poll) {
                                 detail::wait_aws_instance(*state, id, "stopped", timeout, poll);
                             });
        }
        if (provider() == "azure") {
            auto native =
                detail::azure_compute_action(*state_, name, "deallocate", "deallocated");
            const std::string operation_name = native.name;
            const std::string operation_location = native.location;
            return operation(operation_name, operation_location,
                             [state = state_, native = std::move(native)](auto timeout, auto poll) {
                                 detail::wait_azure_compute(*state, native, timeout, poll);
                             });
        }
        return wrap(raw().vm(std::move(name)).stop());
    }
    [[nodiscard]] operation destroy(std::string name) const {
        if (provider() == "aws") {
            const auto native = detail::aws_resolve_instance(*state_, name);
            detail::aws_compute_action(*state_, "TerminateInstances", native.id);
            return operation(native.id, native.zone,
                             [state = state_, id = native.id](auto timeout, auto poll) {
                                 detail::wait_aws_instance(*state, id, "terminated", timeout, poll);
                             });
        }
        if (provider() == "azure") {
            auto native = detail::azure_compute_action(*state_, name, "delete", "deleted");
            const std::string operation_name = native.name;
            const std::string operation_location = native.location;
            return operation(operation_name, operation_location,
                             [state = state_, native = std::move(native)](auto timeout, auto poll) {
                                 detail::wait_azure_compute(*state, native, timeout, poll);
                             });
        }
        return wrap(raw().vm(std::move(name)).erase());
    }

private:
    friend class client;
    explicit compute(std::shared_ptr<detail::client_state> state, cldmux::provider provider)
        : state_(std::move(state)), provider_(std::move(provider)) {}
    [[nodiscard]] const cldmux::provider& provider() const {
        return provider_;
    }
    [[nodiscard]] gcp::Cloud& raw() const {
        if (provider() != "gcp")
            throw error("Unsupported raw-compute provider");
        return state_->raw;
    }
    [[nodiscard]] static operation wrap(gcp::Operation native) {
        const std::string name = native.name();
        const std::string zone = native.zone();
        return operation(name, zone, [native = std::move(native)](auto timeout, auto poll) {
            native.wait(timeout, poll);
        });
    }
    std::shared_ptr<detail::client_state> state_;
    cldmux::provider provider_;
};

class job {
public:
    // Copies share this controller state. status(), logs(), wait(), and cancel()
    // must therefore be serialised by the caller. Provider log backends can expose
    // final entries late, so terminal draining is bounded and best effort. Once a
    // result is cached, status() and logs() return that terminal snapshot.
    [[nodiscard]] const std::string& id() const noexcept { return data_->id; }

    [[nodiscard]] job_state status() const {
        if (data_->cached)
            return data_->cached->state;
        return detail::update(*data_, detail::get_job(*data_));
    }

    [[nodiscard]] std::vector<log_entry> logs() const {
        if (!data_->cached)
            (void)detail::merge_logs(*data_);
        return data_->log_cache;
    }

    [[nodiscard]] result wait(log_sink sink = {}) const {
        // Poll incremental logs and normalised state until terminal or the total
        // controller deadline. Terminal state triggers a quiet-period log drain,
        // result caching, and provider-specific cleanup. At deadline, cancel first.
        // Sink/control exceptions trigger best-effort cancellation and cleanup when
        // enabled, then rethrow the original exception unchanged.
        if (data_->cached)
            return *data_->cached;
        std::string log_warning;
        const auto cleanup = [&](result& out, std::string_view reason) {
            data_->cached = out;
            if (!data_->spec.auto_delete && data_->chosen.provider != "azure")
                return;
            try {
                detail::delete_job(*data_, reason);
            } catch (const std::exception& failure) {
                out.warnings.push_back(std::string("automatic cleanup failed: ") + failure.what());
                data_->cached = out;
            }
        };
        const auto emit = [&](std::chrono::steady_clock::time_point deadline) {
            std::vector<log_entry> added;
            try {
                added = detail::merge_logs(*data_, deadline);
            } catch (const std::exception& failure) {
                log_warning = std::string("log polling failed: ") + failure.what();
                return std::size_t{0};
            }
            for (const auto& line : added)
                if (sink) {
                    try {
                        sink(line);
                    } catch (...) {
                        const auto original = std::current_exception();
                        if (data_->spec.auto_delete || data_->chosen.provider == "azure") {
                            try {
                                const auto json = detail::cancel_job(*data_);
                                result out = detail::make_result(*data_, json);
                                data_->cached = out;
                                detail::delete_job(*data_, "cldmux log callback failure");
                            } catch (...) {
                            }
                        }
                        std::rethrow_exception(original);
                    }
                }
            return added.size();
        };
        const auto drain = [&] { detail::drain_logs(*data_, emit); };
        const auto deadline = data_->submitted + data_->spec.timeout;
        try {
            while (true) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    const auto json = detail::cancel_job(*data_);
                    drain();
                    result out = detail::make_result(*data_, json);
                    if (out.state == job_state::cancelled)
                        out.message = "Cloud job exceeded its controller timeout";
                    if (!log_warning.empty())
                        out.warnings.push_back(log_warning);
                    cleanup(out, "cldmux timeout");
                    return out;
                }
                (void)emit(deadline);
                if (std::chrono::steady_clock::now() >= deadline)
                    continue;
                gcp::detail::Json json;
                try {
                    json = detail::get_job(*data_, deadline);
                } catch (const error&) {
                    if (std::chrono::steady_clock::now() >= deadline)
                        continue;
                    throw;
                }
                const job_state state = detail::update(*data_, json);
                if (detail::terminal(state)) {
                    drain();
                    result out = detail::make_result(*data_, json);
                    if (!log_warning.empty())
                        out.warnings.push_back(log_warning);
                    cleanup(out, "cldmux automatic cleanup");
                    return out;
                }
                detail::pause(*data_->client, deadline);
            }
        } catch (...) {
            const auto original = std::current_exception();
            if ((data_->spec.auto_delete || data_->chosen.provider == "azure") && !data_->cached) {
                try {
                    const auto json = detail::cancel_job(*data_);
                    result out = detail::make_result(*data_, json);
                    cleanup(out, "cldmux control failure");
                } catch (...) {
                }
            }
            std::rethrow_exception(original);
        }
    }

    void cancel() const {
        // Explicit cancellation is idempotent after a terminal result is cached.
        // It still drains delayed logs before performing configured cleanup.
        if (data_->cached)
            return;
        const auto json = detail::cancel_job(*data_);
        std::string warning;
        try {
            detail::drain_logs(*data_, [&](auto deadline) {
                return !detail::merge_logs(*data_, deadline).empty();
            });
        } catch (const std::exception& failure) {
            warning = std::string("final log polling failed: ") + failure.what();
        }
        data_->cached = detail::make_result(*data_, json);
        if (!warning.empty())
            data_->cached->warnings.push_back(std::move(warning));
        if (data_->spec.auto_delete || data_->chosen.provider == "azure") {
            try {
                detail::delete_job(*data_, "cldmux cancellation");
            } catch (const std::exception& failure) {
                data_->cached->warnings.push_back(std::string("automatic cleanup failed: ") +
                                                  failure.what());
            }
        }
    }

private:
    friend class client;
    explicit job(std::shared_ptr<detail::job_data> data) : data_(std::move(data)) {}
    std::shared_ptr<detail::job_data> data_;
};

class router;

// A client is always pinned to one provider. Cheap-copy handles originate from
// one shared client_state, so routing preserves credentials and price caches.
class client {
public:
    [[nodiscard]] cldmux::plan plan(const job_spec& spec) const {
        // Does not mutate provider resources; it may invoke a caller pricing
        // callback or query a read-only public catalogue API.
        return detail::priced_plan(*state_, spec, bound_provider_);
    }

    [[nodiscard]] cldmux::run_diagnostics
    diagnose(const job_spec& spec, std::chrono::milliseconds expected_attempt_runtime) const {
        validate_diagnostic_request(spec, expected_attempt_runtime);
        return diagnose_with_plan(spec, expected_attempt_runtime, plan(spec));
    }

private:
    static void validate_diagnostic_request(
        const job_spec& spec, std::chrono::milliseconds expected_attempt_runtime) {
        // Reject caller input before planning: pricing may invoke a callback or
        // a catalogue request even though diagnostics never allocate resources.
        if (expected_attempt_runtime <= std::chrono::milliseconds::zero())
            throw error("Expected attempt runtime must be positive");
        if (spec.timeout > std::chrono::milliseconds::zero() &&
            expected_attempt_runtime > spec.timeout)
            throw error("Expected attempt runtime must not exceed the controller timeout");
    }

    [[nodiscard]] cldmux::run_diagnostics
    diagnose_with_plan(const job_spec& spec,
                       std::chrono::milliseconds expected_attempt_runtime,
                       cldmux::plan selected_plan) const {
        cldmux::run_diagnostics out;
        out.selected_plan = std::move(selected_plan);
        out.expected_attempt_runtime = expected_attempt_runtime;
        out.controller_timeout = spec.timeout;
        out.configured_retries = spec.retries;
        out.configured_attempt_limit = spec.retries + 1;

        const auto seconds_from = [](std::uint64_t count) {
            using rep = std::chrono::seconds::rep;
            if (count > static_cast<std::uintmax_t>((std::numeric_limits<rep>::max)()))
                throw error("Provider timeout cannot be represented by std::chrono::seconds");
            return std::chrono::seconds(static_cast<rep>(count));
        };
        out.provider_attempt_timeout =
            seconds_from(detail::provider_attempt_timeout_seconds(spec));
        if (out.selected_plan.provider == "azure")
            out.provider_job_timeout =
                seconds_from(detail::azure_job_timeout_seconds(spec, state_->config));

        out.warnings = out.selected_plan.warnings;
        out.warnings.push_back(
            "diagnostics are advisory planning snapshots; run() replans before submission");
        out.warnings.push_back(
            "expected attempt runtime is caller-supplied; queueing and provisioning are not "
            "predicted");
        out.warnings.push_back(
            "cancellation, final log draining, and cleanup can finish after the controller "
            "timeout");
        out.warnings.push_back(
            "controller timeout is enforced only while a live caller is inside wait()");
        if (out.configured_attempt_limit > 1)
            out.warnings.push_back(
                "configured retries can add cost beyond the single-runtime estimate");
        out.warnings.push_back(
            "provider-managed lifecycle can add cost outside the expected-attempt-runtime "
            "estimate");
        if (spec.resources.spot)
            out.warnings.push_back(
                "Spot capacity and future prices are not reserved by planning");
        if (out.selected_plan.provider == "gcp")
            out.warnings.push_back("GCP applies its provider timeout separately to each attempt");
        else if (out.selected_plan.provider == "aws") {
            out.warnings.push_back(
                "AWS Batch does not retry attempts terminated by its provider timeout");
            out.warnings.push_back(
                "AWS provider timeout excludes queueing and STARTING and termination is best "
                "effort");
        } else if (out.selected_plan.provider == "azure") {
            out.warnings.push_back(
                "Azure's job watchdog includes cleanup, final-log, and request allowances");
            out.warnings.push_back(
                "Azure applies its task timeout separately to each attempt");
            out.warnings.push_back(
                "Azure system recovery can occur outside the configured attempt limit");
            if (spec.resources.spot)
                out.warnings.push_back(
                    "Azure Spot preemption can requeue a task outside the configured attempt "
                    "limit");
        }

        if (out.selected_plan.estimated_hourly_cost) {
            const double hourly = *out.selected_plan.estimated_hourly_cost;
            if (state_->config.lookup_hourly_cost || state_->config.estimate_hourly_cost)
                out.warnings.push_back(
                    "included charges follow the caller's hourly callback; the library adds "
                    "none");
            else
                out.warnings.push_back(
                    "public-catalogue compute cost excludes storage, disks, network, licences, "
                    "taxes, discounts, and provider overhead");
            const auto cost_for_hours = [hourly](double hours) {
                const double cost = hourly * hours;
                if (!std::isfinite(cost))
                    throw error("Diagnostic cost is not finite");
                return cost;
            };
            const double expected_hours =
                std::chrono::duration<double, std::chrono::hours::period>(
                    expected_attempt_runtime)
                    .count();
            out.estimated_cost_for_expected_attempt_runtime = cost_for_hours(expected_hours);
        }
        return out;
    }

public:
    [[nodiscard]] const cldmux::provider& selected_provider() const {
        return bound_provider_;
    }

    [[nodiscard]] cldmux::job run(const job_spec& spec) const {
        // Replan immediately before submission so validation and price ceilings
        // are current. GCP uses one randomised audit name plus requestId and, after
        // an ambiguous create response, recovers with GET-by-name instead of replaying.
        const cldmux::plan chosen = plan(spec);
        const std::string id = detail::job_id(spec.name);
        if (chosen.provider == "aws")
            return cldmux::job(detail::submit_aws(state_, spec, chosen, id));
        if (chosen.provider == "azure")
            return cldmux::job(detail::submit_azure(state_, spec, chosen, id));
        const std::string project = state_->core()->project();
        detail::validate_project(project);
        const std::string parent = "projects/" + project + "/locations/" + chosen.region + "/jobs";
        const std::string name = parent + '/' + id;
        const std::string request_id = gcp::detail::random_uuid();
        const std::string create_url =
            state_->core()->config.batch_endpoint + "/v1/projects/" + gcp::detail::encode(project) +
            "/locations/" + gcp::detail::encode(chosen.region) + "/jobs" +
            "?jobId=" + gcp::detail::encode(id) + "&requestId=" + request_id;
        const std::string body = detail::batch_body(spec, chosen);
        gcp::detail::HttpResponse created_response;
        try {
            created_response =
                detail::call(*state_, gcp::detail::HttpRequest{}
                                          .with_method("POST")
                                          .with_url(create_url)
                                          .with_headers({"Content-Type: application/json"})
                                          .with_body(body));
        } catch (const error& failure) {
            if (!detail::retryable(failure.http_status()))
                throw;
            const auto submission = std::current_exception();
            const auto recovery =
                std::min(state_->config.cleanup_timeout, std::chrono::milliseconds(30'000));
            const auto deadline = std::chrono::steady_clock::now() + recovery;
            while (true) {
                try {
                    created_response =
                        detail::call(*state_,
                                     gcp::detail::HttpRequest{}.with_url(
                                         state_->core()->config.batch_endpoint + "/v1/" + name),
                                     deadline);
                    break;
                } catch (const error& lookup) {
                    if (lookup.http_status() != 404 || std::chrono::steady_clock::now() >= deadline)
                        std::rethrow_exception(submission);
                    detail::pause(*state_, deadline);
                }
            }
        }
        const auto created = gcp::detail::parse_json(created_response.body);
        auto data = std::make_shared<detail::job_data>();
        data->client = state_;
        data->spec = spec;
        data->chosen = chosen;
        data->id = id;
        data->name = gcp::detail::field(created, "name");
        if (data->name.empty())
            data->name = name;
        data->uid = gcp::detail::field(created, "uid");
        return cldmux::job(std::move(data));
    }

    [[nodiscard]] bool supports(feature requested) const {
        // Capability reports implemented behaviour, not a live account, quota,
        // queue, regional SKU, or credential probe.
        return detail::supports(bound_provider_, requested);
    }

    [[nodiscard]] cldmux::storage& storage() noexcept { return storage_; }
    [[nodiscard]] const cldmux::storage& storage() const noexcept { return storage_; }
    [[nodiscard]] cldmux::compute& compute() noexcept { return compute_; }
    [[nodiscard]] const cldmux::compute& compute() const noexcept { return compute_; }

private:
    friend class router;

    client(std::shared_ptr<detail::client_state> state, cldmux::provider selected_provider)
        : state_(std::move(state)), bound_provider_(std::move(selected_provider)),
          storage_(state_, bound_provider_), compute_(state_, bound_provider_) {}

    std::shared_ptr<detail::client_state> state_;
    cldmux::provider bound_provider_;
    cldmux::storage storage_;
    cldmux::compute compute_;
};

} // namespace cldmux
