#pragma once

#include "cldmux/detail/provider.hpp"

namespace cldmux {
namespace detail {

// Azure Resource Manager raw-instance control --------------------------------

inline std::string azure_management_endpoint(const client_state& client) {
    const std::string endpoint =
        gcp::detail::base_url(client.config.azure.management_endpoint);
    if (endpoint.empty())
        throw error("Azure raw compute requires config::azure.management_endpoint");
    validate_endpoint(client, endpoint, "Azure management_endpoint");
    if (client.config.azure.subscription_id.empty() || client.config.azure.resource_group.empty())
        throw error("Azure raw compute requires a subscription ID and resource group");
    return endpoint;
}

inline std::string azure_vm_collection_url(const client_state& client) {
    return azure_management_endpoint(client) + "/subscriptions/" +
           gcp::detail::encode(client.config.azure.subscription_id) + "/resourceGroups/" +
           gcp::detail::encode(client.config.azure.resource_group) +
           "/providers/Microsoft.Compute/virtualMachines";
}

inline std::string azure_vm_url(const client_state& client, std::string_view name,
                                std::string_view action = {}) {
    std::string result = azure_vm_collection_url(client) + '/' + gcp::detail::encode(name);
    if (!action.empty())
        result += '/' + std::string(action);
    result += "?api-version=" + gcp::detail::encode(client.config.azure.compute_api_version);
    return result;
}

inline std::string azure_power_state(
    const client_state& client, std::string_view name,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    const auto json = gcp::detail::parse_json(
        azure_management_call(
            client, gcp::detail::HttpRequest{}.with_url(azure_vm_url(client, name, "instanceView")),
            true, deadline)
            .body);
    const auto* view = json.get("statuses");
    if (!view) {
        const auto* properties = json.get("properties");
        view = properties ? properties->get("statuses") : nullptr;
    }
    if (view)
        for (const auto& status : view->array()) {
            const std::string code = gcp::detail::field(status, "code");
            constexpr std::string_view prefix = "PowerState/";
            if (gcp::detail::starts_with(code, prefix))
                return code.substr(prefix.size());
        }
    return "unknown";
}

inline instance parse_azure_instance(const client_state& client,
                                     const gcp::detail::Json& json) {
    instance result;
    result.id = gcp::detail::field(json, "id");
    result.name = gcp::detail::field(json, "name");
    result.zone = gcp::detail::field(json, "location");
    if (const auto* properties = json.get("properties")) {
        if (const auto* hardware = properties->get("hardwareProfile"))
            result.machine_type = gcp::detail::field(*hardware, "vmSize");
        result.creation_timestamp = gcp::detail::field(*properties, "timeCreated");
        result.status = gcp::detail::field(*properties, "provisioningState");
    }
    if (result.name.empty())
        throw error("Malformed Azure VM response: missing name");
    try {
        result.status = azure_power_state(client, result.name);
    } catch (const error& failure) {
        if (failure.http_status() != 404)
            throw;
    }
    return result;
}

inline std::vector<instance> azure_compute_instances(const client_state& client,
                                                     std::size_t limit = 0) {
    std::vector<instance> result;
    std::string url = azure_vm_collection_url(client) + "?api-version=" +
                      gcp::detail::encode(client.config.azure.compute_api_version);
    for (int page = 0; page < 1000 && !url.empty(); ++page) {
        const auto json = gcp::detail::parse_json(
            azure_management_call(client, gcp::detail::HttpRequest{}.with_url(url)).body);
        if (const auto* values = json.get("value"))
            for (const auto& item : values->array()) {
                result.push_back(parse_azure_instance(client, item));
                if (limit && result.size() == limit)
                    return result;
            }
        const std::string next = gcp::detail::field(json, "nextLink");
        if (next == url)
            throw error("Azure VM pagination did not advance");
        url = next;
    }
    if (!url.empty())
        throw error("Azure VM listing exceeded 1000 pages");
    return result;
}

struct azure_compute_operation {
    std::string name;
    std::string location;
    std::string poll_url;
    std::string target;
    bool status_url = false;
};

inline azure_compute_operation azure_operation(const gcp::detail::HttpResponse& response,
                                                std::string name, std::string target,
                                                std::string location = {}) {
    std::string poll = response_header(response, "azure-asyncoperation");
    const bool status_url = !poll.empty();
    if (!status_url)
        poll = response_header(response, "location");
    return {std::move(name), std::move(location), std::move(poll), std::move(target), status_url};
}

inline azure_compute_operation azure_compute_create(const client_state& client,
                                                     std::string_view name,
                                                     std::string_view logical_template) {
    if (name.empty())
        throw error("Raw instance name must not be empty");
    const auto configured = client.config.instance_templates.find(logical_template);
    if (configured == client.config.instance_templates.end())
        throw error("Unknown logical compute template: " + std::string(logical_template));
    const auto& native = configured->second.azure;
    if (native.image_id.empty() || native.subnet_id.empty() || native.machine_type.empty())
        throw error("Azure logical compute template requires image, subnet, and machine type");
    const std::string location =
        region(native.location.empty() ? configured_region(client.config, "azure")
                                       : native.location,
               "azure");
    const std::string vm_name(name);
    const std::string body =
        "{\"location\":" + gcp::detail::json_quote(location) +
        ",\"properties\":{\"hardwareProfile\":{\"vmSize\":" +
        gcp::detail::json_quote(native.machine_type) +
        "},\"storageProfile\":{\"imageReference\":{\"id\":" +
        gcp::detail::json_quote(native.image_id) +
        "},\"osDisk\":{\"name\":" + gcp::detail::json_quote(vm_name + "-os") +
        ",\"createOption\":\"FromImage\",\"deleteOption\":\"Delete\","
        "\"managedDisk\":{\"storageAccountType\":" +
        gcp::detail::json_quote(native.os_disk_type) +
        "}}},\"networkProfile\":{\"networkApiVersion\":\"2022-11-01\","
        "\"networkInterfaceConfigurations\":[{\"name\":" +
        gcp::detail::json_quote(vm_name + "-nic") +
        ",\"properties\":{\"primary\":true,\"deleteOption\":\"Delete\","
        "\"ipConfigurations\":[{\"name\":\"ipconfig\",\"properties\":{"
        "\"primary\":true,\"subnet\":{\"id\":" +
        gcp::detail::json_quote(native.subnet_id) + "}}}]}}]}}}";
    const auto response = azure_management_call(
        client,
        gcp::detail::HttpRequest{}
            .with_method("PUT")
            .with_url(azure_vm_url(client, name))
            .with_headers({"Content-Type: application/json", "If-None-Match: *"})
            .with_body(body),
        false);
    return azure_operation(response, vm_name, "running", location);
}

inline azure_compute_operation azure_compute_action(const client_state& client,
                                                     std::string_view name,
                                                     std::string_view action,
                                                     std::string target) {
    const auto response = azure_management_call(
        client, gcp::detail::HttpRequest{}
                    .with_method(action == "delete" ? "DELETE" : "POST")
                    .with_url(action == "delete" ? azure_vm_url(client, name)
                                                 : azure_vm_url(client, name, action)),
        false);
    return azure_operation(response, std::string(name), std::move(target));
}

inline void wait_azure_compute(const client_state& client, azure_compute_operation operation,
                               std::chrono::milliseconds timeout,
                               std::chrono::milliseconds poll_interval) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!operation.poll_url.empty()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw error("Timed out waiting for Azure VM operation '" + operation.name + "'");
        const auto response = azure_management_call(
            client, gcp::detail::HttpRequest{}.with_url(operation.poll_url), true, deadline);
        bool completed = false;
        if (operation.status_url) {
            if (!response.body.empty()) {
                const auto json = gcp::detail::parse_json(response.body);
                std::string status = gcp::detail::field(json, "status");
                if (status.empty())
                    if (const auto* properties = json.get("properties"))
                        status = gcp::detail::field(*properties, "provisioningState");
                if (status == "Failed" || status == "Canceled" || status == "Cancelled")
                    throw error("Azure VM operation failed with state " + status);
                completed = status == "Succeeded";
            }
        } else {
            // A Location URL may finish with an empty 200/201/204 or with the
            // resulting resource rather than an operation-status document.
            completed = response.status != 202;
        }
        if (completed) {
            if (operation.target == "deleted")
                return;
            operation.poll_url.clear();
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
            throw error("Timed out waiting for Azure VM operation '" + operation.name + "'");
        auto delay = poll_interval;
        const std::string retry_after = response_header(response, "retry-after");
        if (!retry_after.empty()) {
            try {
                const auto seconds = unsigned_text(retry_after, "Retry-After");
                if (seconds <= static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max() / 1000))
                    delay = std::max(
                        delay, std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000)));
            } catch (const error&) {
                // HTTP-date Retry-After values are uncommon for ARM; fall back
                // to the caller's bounded polling interval.
            }
        }
        std::this_thread::sleep_for(std::min(delay, remaining));
    }
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw error("Timed out waiting for Azure VM '" + operation.name + "'");
        try {
            const std::string state = azure_power_state(client, operation.name, deadline);
            if (state == operation.target)
                return;
        } catch (const error& failure) {
            if (operation.target == "deleted" && failure.http_status() == 404)
                return;
            throw;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
            throw error("Timed out waiting for Azure VM '" + operation.name + "'");
        std::this_thread::sleep_for(std::min(poll_interval, remaining));
    }
}

} // namespace detail
} // namespace cldmux
