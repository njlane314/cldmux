#pragma once

#include "cloud/detail/provider.hpp"

namespace cloud {
namespace detail {

// AWS EC2 raw-instance control ------------------------------------------------

inline std::string aws_ec2_endpoint(const client_state& client) {
    const std::string selected_region = region(configured_region(client.config, "aws"), "aws");
    return aws_endpoint(client, client.config.aws.ec2_endpoint, "ec2", selected_region);
}

inline gcp::detail::HttpResponse aws_ec2_call(const client_state& client, std::string body,
                                              bool retry = true,
                                              std::chrono::steady_clock::time_point deadline =
                                                  std::chrono::steady_clock::time_point::max()) {
    const std::string selected_region = region(configured_region(client.config, "aws"), "aws");
    return aws_call(client,
                    gcp::detail::HttpRequest{}
                        .with_method("POST")
                        .with_url(aws_ec2_endpoint(client) + '/')
                        .with_headers({"Content-Type: application/x-www-form-urlencoded"})
                        .with_body(std::move(body))
                        .with_accept_json(false),
                    selected_region, "ec2", retry, deadline);
}

inline instance parse_aws_instance(std::string_view xml) {
    instance result;
    result.id = xml_field(xml, "instanceId");
    result.name = result.id;
    const std::string tags = xml_raw_field(xml, "tagSet");
    for (const auto tag : xml_blocks(tags, "item"))
        if (xml_field(tag, "key") == "Name")
            result.name = xml_field(tag, "value");
    result.zone = xml_field(xml_raw_field(xml, "placement"), "availabilityZone");
    result.machine_type = xml_field(xml, "instanceType");
    result.status = xml_field(xml_raw_field(xml, "instanceState"), "name");
    result.internal_ip = xml_field(xml, "privateIpAddress");
    result.external_ip = xml_field(xml, "ipAddress");
    result.creation_timestamp = xml_field(xml, "launchTime");
    if (result.id.empty())
        throw error("Malformed AWS EC2 instance response: missing instanceId");
    return result;
}

inline std::vector<instance> aws_instances_from_xml(std::string_view xml) {
    std::vector<instance> result;
    for (const auto instances : xml_blocks(xml, "instancesSet"))
        for (const auto item : xml_blocks(instances, "item"))
            result.push_back(parse_aws_instance(item));
    return result;
}

inline std::vector<instance> aws_compute_instances(const client_state& client,
                                                   std::size_t limit = 0) {
    std::vector<instance> result;
    std::string token;
    for (int page = 0; page < 1000; ++page) {
        std::string body = "Action=DescribeInstances&Version=2016-11-15&MaxResults=1000";
        if (!token.empty())
            body += "&NextToken=" + gcp::detail::encode(token);
        const auto response = aws_ec2_call(client, std::move(body));
        auto found = aws_instances_from_xml(response.body);
        if (limit && found.size() > limit - result.size())
            found.resize(limit - result.size());
        result.insert(result.end(), std::make_move_iterator(found.begin()),
                      std::make_move_iterator(found.end()));
        if (limit && result.size() == limit)
            return result;
        const std::string next = xml_field(response.body, "nextToken");
        if (next.empty())
            return result;
        if (next == token)
            throw error("AWS EC2 instance pagination did not advance");
        token = next;
    }
    throw error("AWS EC2 instance listing exceeded 1000 pages");
}

inline std::vector<instance> aws_describe_instance(const client_state& client,
                                                   std::string_view id,
                                                   std::chrono::steady_clock::time_point deadline =
                                                       std::chrono::steady_clock::time_point::max()) {
    const std::string body =
        "Action=DescribeInstances&Version=2016-11-15&InstanceId.1=" +
        gcp::detail::encode(id);
    return aws_instances_from_xml(aws_ec2_call(client, body, true, deadline).body);
}

inline std::vector<instance> aws_named_instances(const client_state& client,
                                                 std::string_view name) {
    std::vector<instance> result;
    std::string token;
    for (int page = 0; page < 1000; ++page) {
        std::string body =
            "Action=DescribeInstances&Version=2016-11-15&MaxResults=1000&"
            "Filter.1.Name=tag%3AName&Filter.1.Value.1=" +
            gcp::detail::encode(name) +
            "&Filter.2.Name=tag%3Acloud-hpp&Filter.2.Value.1=managed";
        if (!token.empty())
            body += "&NextToken=" + gcp::detail::encode(token);
        const auto response = aws_ec2_call(client, std::move(body));
        for (auto& found : aws_instances_from_xml(response.body)) {
            // EC2 keeps terminated instances visible for a while. They no
            // longer reserve the portable logical name.
            if (found.status != "terminated")
                result.push_back(std::move(found));
            // Two live matches are enough to fail every name-based operation.
            if (result.size() == 2)
                return result;
        }
        const std::string next = xml_field(response.body, "nextToken");
        if (next.empty())
            return result;
        if (next == token)
            throw error("AWS EC2 managed-name pagination did not advance");
        token = next;
    }
    throw error("AWS EC2 managed-name lookup exceeded 1000 pages");
}

inline bool is_aws_instance_id(std::string_view value) {
    // EC2 IDs use i- plus either the historical 8 or current 17 hexadecimal
    // digits. A logical name such as i-worker must remain a Name-tag lookup.
    if (!gcp::detail::starts_with(value, "i-") || (value.size() != 10 && value.size() != 19))
        return false;
    return std::all_of(value.begin() + 2, value.end(),
                       [](char digit) { return hex_digit(digit) >= 0; });
}

inline instance aws_resolve_instance(const client_state& client, std::string_view name) {
    auto found = is_aws_instance_id(name) ? aws_describe_instance(client, name)
                                          : aws_named_instances(client, name);
    if (found.empty())
        throw error("AWS EC2 instance name was not found: " + std::string(name));
    if (found.size() != 1)
        throw error("AWS EC2 instance name is ambiguous: " + std::string(name));
    return std::move(found.front());
}

inline instance aws_compute_create(const client_state& client, std::string_view name,
                                   std::string_view logical_template) {
    if (name.empty())
        throw error("Raw instance name must not be empty");
    if (is_aws_instance_id(name))
        throw error("AWS logical instance names must not use EC2 instance-ID syntax");
    const auto configured = client.config.instance_templates.find(logical_template);
    if (configured == client.config.instance_templates.end())
        throw error("Unknown logical compute template: " + std::string(logical_template));
    const auto& native = configured->second.aws;
    if (native.id.empty() == native.name.empty())
        throw error("AWS logical compute template requires exactly one launch template ID or "
                    "name");
    if (native.version.empty())
        throw error("AWS launch template version must not be empty");
    // EC2 Name tags are not unique. This preflight prevents ordinary duplicate
    // creation while acknowledging that two concurrent controllers can still
    // race; subsequent name-based actions fail closed if that happens.
    if (!aws_named_instances(client, name).empty())
        throw error("AWS EC2 managed instance name already exists: " + std::string(name));
    std::string body =
        "Action=RunInstances&Version=2016-11-15&MinCount=1&MaxCount=1&ClientToken=" +
        gcp::detail::encode(gcp::detail::random_uuid());
    body += native.id.empty() ? "&LaunchTemplate.LaunchTemplateName=" +
                                    gcp::detail::encode(native.name)
                              : "&LaunchTemplate.LaunchTemplateId=" +
                                    gcp::detail::encode(native.id);
    body += "&LaunchTemplate.Version=" + gcp::detail::encode(native.version) +
            "&TagSpecification.1.ResourceType=instance&TagSpecification.1.Tag.1.Key=Name&"
            "TagSpecification.1.Tag.1.Value=" +
            gcp::detail::encode(name) +
            "&TagSpecification.1.Tag.2.Key=cloud-hpp&"
            "TagSpecification.1.Tag.2.Value=managed";
    const auto instances = aws_instances_from_xml(aws_ec2_call(client, std::move(body)).body);
    if (instances.size() != 1)
        throw error("Malformed AWS RunInstances response: expected one instance");
    return instances.front();
}

inline void aws_compute_action(const client_state& client, std::string_view action,
                               std::string_view id) {
    (void)aws_ec2_call(client, "Action=" + std::string(action) +
                                  "&Version=2016-11-15&InstanceId.1=" +
                                  gcp::detail::encode(id));
}

inline void wait_aws_instance(const client_state& client, std::string id, std::string target,
                              std::chrono::milliseconds timeout,
                              std::chrono::milliseconds poll_interval) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            throw error("Timed out waiting for AWS EC2 instance '" + id + "'");
        std::vector<instance> found;
        try {
            found = aws_describe_instance(client, id, deadline);
        } catch (const error& failure) {
            const bool temporarily_missing =
                failure.http_status() == 400 &&
                failure.response().find("InvalidInstanceID.NotFound") != std::string::npos;
            if (!temporarily_missing)
                throw;
            // EC2 documents eventual consistency immediately after RunInstances.
            // Keep polling rather than turning this transient 400 into failure.
        }
        if (!found.empty() && found.front().status == target) {
            return;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
            throw error("Timed out waiting for AWS EC2 instance '" + id + "'");
        std::this_thread::sleep_for(std::min(poll_interval, remaining));
    }
}

} // namespace detail
} // namespace cloud
