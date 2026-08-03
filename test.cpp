#include <cloud>

#if __cplusplus >= 202002L
#include <tst.hpp>
#else
// tst.hpp deliberately targets C++20 because it reports assertion locations
// with std::source_location. This small test-only fallback keeps this library's
// own C++17 build exercising the same cases without changing the sibling tst
// project or weakening its public contract.
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace tst {

namespace detail {

struct failure : std::runtime_error {
    explicit failure(std::string_view message) : std::runtime_error(std::string(message)) {}
};

template <class Action> bool run_one(std::string_view name, Action&& action) {
    try {
        std::forward<Action>(action)();
    } catch (const failure& error) {
        std::cout << "FAIL  " << name << '\n' << error.what() << '\n';
        return false;
    } catch (const std::exception& error) {
        std::cout << "ERROR " << name << "\nunexpected exception: " << error.what() << '\n';
        return false;
    } catch (...) {
        std::cout << "ERROR " << name << "\nunexpected non-standard exception\n";
        return false;
    }
    std::cout << "PASS  " << name << '\n';
    return true;
}

} // namespace detail

template <class Condition>
void check(Condition&& condition, std::string_view message = "check failed") {
    if (!static_cast<bool>(std::forward<Condition>(condition)))
        throw detail::failure(message);
}

template <class Error, class Action>
void throws(Action&& action, std::string_view message = "expected exception was not thrown") {
    try {
        std::forward<Action>(action)();
    } catch (const detail::failure&) {
        throw;
    } catch (const Error&) {
        return;
    }
    throw detail::failure(message);
}

template <class Action> struct test {
    std::string name;
    Action action;
};

template <class Action> test<Action> make_test(std::string name, Action action) {
    return {std::move(name), std::move(action)};
}

template <class... Tests> int run(Tests&&... tests) {
    int passed = 0;
    int failed = 0;
    ((detail::run_one(tests.name, std::forward<Tests>(tests).action) ? ++passed : ++failed), ...);
    std::cout << '\n' << passed << " passed, " << failed << " failed\n";
    return failed != 0;
}

} // namespace tst

#define TST_CASE(name, ...) ::tst::make_test(name, [&] { __VA_ARGS__; })
#endif

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class comma_decimal_point : public std::numpunct<char> {
protected:
    char do_decimal_point() const override { return ','; }
};

class global_locale_guard {
public:
    explicit global_locale_guard(std::locale replacement) : previous_(std::locale()) {
        std::locale::global(replacement);
    }

    global_locale_guard(const global_locale_guard&) = delete;
    global_locale_guard& operator=(const global_locale_guard&) = delete;

    ~global_locale_guard() { std::locale::global(previous_); }

private:
    std::locale previous_;
};

// C++20 added starts_with() and ends_with() to string_view. Keeping these
// compatibility helpers local to the tests avoids changing their intent.
bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Tests alter process-wide environment state, so every permitted name is saved
// before use and restored even when an assertion throws.
class environment_guard {
public:
    environment_guard(std::initializer_list<std::string_view> names) {
        for (const std::string_view name : names) {
            const std::string key(name);
            const char* value = std::getenv(key.c_str());
            saved_.push_back({key, value ? std::optional<std::string>(value) : std::nullopt});
        }
        for (const auto& item : saved_)
            if (::unsetenv(item.name.c_str()) != 0) {
                restore();
                throw std::runtime_error("cannot clear test environment variable " + item.name);
            }
    }

    environment_guard(const environment_guard&) = delete;
    environment_guard& operator=(const environment_guard&) = delete;

    ~environment_guard() { restore(); }

    void set(std::string_view name, std::string_view value) {
        const auto& item = saved(name);
        const std::string text(value);
        if (::setenv(item.name.c_str(), text.c_str(), 1) != 0)
            throw std::runtime_error("cannot set test environment variable " + item.name);
    }

    void unset(std::string_view name) {
        const auto& item = saved(name);
        if (::unsetenv(item.name.c_str()) != 0)
            throw std::runtime_error("cannot clear test environment variable " + item.name);
    }

private:
    void restore() noexcept {
        for (const auto& item : saved_) {
            if (item.value)
                (void)::setenv(item.name.c_str(), item.value->c_str(), 1);
            else
                (void)::unsetenv(item.name.c_str());
        }
    }
    struct saved_variable {
        std::string name;
        std::optional<std::string> value;
    };

    [[nodiscard]] const saved_variable& saved(std::string_view name) const {
        const auto found = std::find_if(saved_.begin(), saved_.end(),
                                        [&](const auto& item) { return item.name == name; });
        if (found == saved_.end())
            throw std::logic_error("unsaved test environment variable " + std::string(name));
        return *found;
    }

    std::vector<saved_variable> saved_;
};

class fake_server {
public:
    fake_server() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0)
            throw std::runtime_error("socket");
        int reuse = 1;
        (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                           static_cast<socklen_t>(sizeof(reuse)));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address),
                   static_cast<socklen_t>(sizeof(address))) != 0 ||
            ::listen(listener_, 16) != 0)
            throw std::runtime_error("listen");
        socklen_t length = static_cast<socklen_t>(sizeof(address));
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            throw std::runtime_error("getsockname");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    fake_server(const fake_server&) = delete;
    fake_server& operator=(const fake_server&) = delete;

    ~fake_server() {
        stopping_ = true;
        (void)::shutdown(listener_, SHUT_RDWR);
        (void)::close(listener_);
        if (thread_.joinable())
            thread_.join();
    }

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::atomic<int> create_retries{0};
    std::atomic<int> ambiguous_cancel_attempts{0};
    std::atomic<int> cancel_operations{0};
    std::atomic<int> deletes{0};
    std::atomic<int> cursor_queries{0};
    std::atomic<int> overlap_queries{0};
    std::atomic<int> storage_lists{0};
    std::atomic<int> aws_registers{0};
    std::atomic<int> aws_definition_reads{0};
    std::atomic<int> aws_submits{0};
    std::atomic<int> aws_deregisters{0};
    std::atomic<int> aws_log_reads{0};
    std::atomic<int> aws_cancel_mode{0};
    std::atomic<int> aws_cancel_requests{0};
    std::atomic<bool> aws_cancel_accepted{false};
    std::atomic<int> azure_jobs{0};
    std::atomic<int> azure_tasks{0};
    std::atomic<int> azure_deletes{0};
    std::atomic<int> azure_delete_polls{0};
    std::atomic<int> azure_terminates{0};
    std::atomic<int> azure_task_reads{0};
    std::atomic<int> gcp_price_reads{0};
    std::atomic<int> aws_price_reads{0};
    std::atomic<int> aws_fargate_price_reads{0};
    std::atomic<int> azure_price_reads{0};
    std::atomic<int> aws_spot_reads{0};
    std::atomic<int> aws_storage_puts{0};
    std::atomic<int> aws_storage_file_puts{0};
    std::atomic<int> aws_storage_gets{0};
    std::atomic<int> aws_storage_lists{0};
    std::atomic<int> aws_storage_deletes{0};
    std::atomic<int> aws_fargate_registers{0};
    std::atomic<int> aws_fargate_submits{0};
    std::atomic<int> azure_storage_puts{0};
    std::atomic<int> azure_storage_file_puts{0};
    std::atomic<int> azure_storage_gets{0};
    std::atomic<int> azure_storage_lists{0};
    std::atomic<int> azure_storage_deletes{0};
    std::atomic<int> azure_mount_jobs{0};
    std::atomic<int> azure_mount_tasks{0};
    std::atomic<int> gcp_compute_creates{0};
    std::atomic<int> aws_compute_lists{0};
    std::atomic<int> aws_compute_create_attempts{0};
    std::atomic<int> aws_compute_create_polls{0};
    std::atomic<int> aws_compute_starts{0};
    std::atomic<int> aws_compute_start_polls{0};
    std::atomic<int> aws_compute_stops{0};
    std::atomic<int> aws_compute_stop_polls{0};
    std::atomic<int> aws_compute_destroys{0};
    std::atomic<int> aws_compute_destroy_polls{0};
    std::atomic<int> azure_compute_lists{0};
    std::atomic<int> azure_compute_creates{0};
    std::atomic<int> azure_compute_starts{0};
    std::atomic<int> azure_compute_deallocates{0};
    std::atomic<int> azure_compute_deletes{0};
    std::atomic<int> azure_compute_operation_polls{0};
    std::atomic<bool> aws_body_valid{true};
    std::atomic<bool> aws_fargate_price_valid{true};
    std::atomic<bool> aws_storage_valid{true};
    std::atomic<bool> aws_mount_body_valid{true};
    std::atomic<bool> aws_compute_valid{true};
    std::atomic<bool> aws_ambiguous_register{false};
    std::atomic<bool> aws_endless_logs{false};
    std::atomic<bool> azure_body_valid{true};
    std::atomic<bool> azure_storage_valid{true};
    std::atomic<bool> azure_mount_body_valid{true};
    std::atomic<bool> azure_compute_valid{true};
    std::atomic<bool> gcp_compute_valid{true};
    std::atomic<bool> azure_cross_origin_next{false};
    std::atomic<bool> azure_cross_origin_operation{false};
    std::atomic<bool> azure_cross_origin_price{false};
    std::atomic<bool> aws_compute_retry_wait{false};
    std::atomic<bool> azure_compute_retry_wait{false};
    std::atomic<bool> corrupt_download{false};
    std::atomic<bool> request_id_changed{false};

private:
    struct reply {
        int status = 200;
        std::string body = "{}";
        std::vector<std::string> headers;

        reply() = default;
        reply(int status_value, std::string body_value)
            : status(status_value), body(std::move(body_value)) {}
        reply(int status_value, std::string body_value, std::vector<std::string> header_values)
            : status(status_value), body(std::move(body_value)),
              headers(std::move(header_values)) {}
    };

    static std::string request_header(std::string_view headers, std::string_view name) {
        std::string wanted(name);
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        std::size_t cursor = 0;
        while (cursor < headers.size()) {
            const auto end = headers.find("\r\n", cursor);
            const auto line = headers.substr(cursor, end - cursor);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string field(line.substr(0, colon));
                std::transform(field.begin(), field.end(), field.begin(), [](char c) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                });
                if (field == wanted) {
                    const auto begin = line.find_first_not_of(" \t", colon + 1);
                    return begin == std::string_view::npos ? std::string{}
                                                           : std::string(line.substr(begin));
                }
            }
            if (end == std::string_view::npos)
                break;
            cursor = end + 2;
        }
        return {};
    }

    static bool has_header(std::string_view headers, std::string_view name,
                           std::string_view value) {
        return request_header(headers, name) == value;
    }

    static std::string query(std::string_view target, std::string_view key) {
        const std::string needle = std::string(key) + '=';
        const auto begin = target.find(needle);
        if (begin == std::string_view::npos)
            return {};
        const auto value = begin + needle.size();
        const auto end = target.find('&', value);
        return std::string(target.substr(value, end - value));
    }

    static std::string job_id(std::string_view target) {
        constexpr std::string_view marker = "/jobs/";
        const auto begin = target.find(marker);
        if (begin == std::string_view::npos)
            return {};
        const auto value = begin + marker.size();
        const auto end = target.find_first_of("?:", value);
        return std::string(target.substr(value, end - value));
    }

    static std::string gcp_sku(std::string_view description, std::string_view usage,
                               unsigned nanos) {
        return "{\"description\":" + cloud::gcp::detail::json_quote(description) +
               ",\"serviceRegions\":[\"europe-west4\"],\"category\":{\"usageType\":" +
               cloud::gcp::detail::json_quote(usage) +
               "},\"pricingInfo\":[{\"pricingExpression\":{\"tieredRates\":[{\"unitPrice\":{"
               "\"currencyCode\":\"USD\",\"units\":\"0\",\"nanos\":" +
               std::to_string(nanos) + "}}]}}]}";
    }

    static std::string aws_fargate_product(std::string_view usage_type,
                                           std::string_view resource_name,
                                           std::string_view resource_value, std::string_view price,
                                           std::string_view extra_attributes = {}) {
        const std::string offer =
            "{\"product\":{\"productFamily\":\"Compute\",\"attributes\":{"
            "\"servicecode\":\"AmazonECS\",\"regionCode\":\"eu-west-1\","
            "\"tenancy\":\"Shared\",\"operation\":\"\",\"usagetype\":" +
            cloud::gcp::detail::json_quote(usage_type) + "," +
            cloud::gcp::detail::json_quote(resource_name) + ':' +
            cloud::gcp::detail::json_quote(resource_value) + std::string(extra_attributes) +
            "}},\"serviceCode\":\"AmazonECS\",\"terms\":{\"OnDemand\":{\"term\":{"
            "\"priceDimensions\":{\"dimension\":{\"unit\":\"hours\","
            "\"beginRange\":\"0\",\"endRange\":\"Inf\",\"pricePerUnit\":{\"USD\":" +
            cloud::gcp::detail::json_quote(price) + "}}}}}}}";
        return cloud::gcp::detail::json_quote(offer);
    }

    static std::string aws_instance_xml(std::string_view id, std::string_view name,
                                        std::string_view state) {
        return "<item><instanceId>" + std::string(id) +
               "</instanceId><instanceType>m7i.large</instanceType>"
               "<instanceState><name>" +
               std::string(state) +
               "</name></instanceState><placement><availabilityZone>eu-west-1a"
               "</availabilityZone></placement><privateIpAddress>10.0.0.4</privateIpAddress>"
               "<ipAddress>198.51.100.4</ipAddress>"
               "<launchTime>2026-08-03T10:00:00Z</launchTime>"
               "<tagSet><item><key>Name</key><value>" +
               std::string(name) + "</value></item></tagSet></item>";
    }

    static std::string aws_instances_xml(std::string instances, std::string next = {}) {
        return "<DescribeInstancesResponse><reservationSet><item><instancesSet>" + instances +
               "</instancesSet></item></reservationSet>" +
               (next.empty() ? std::string{} : "<nextToken>" + next + "</nextToken>") +
               "</DescribeInstancesResponse>";
    }

    static std::string azure_vm_json(std::string_view name, std::string_view state) {
        return "{\"id\":\"/subscriptions/subscription/resourceGroups/workers/providers/"
               "Microsoft.Compute/virtualMachines/" +
               std::string(name) + "\",\"name\":" + cloud::gcp::detail::json_quote(name) +
               ",\"location\":\"westeurope\",\"properties\":{\"hardwareProfile\":{"
               "\"vmSize\":\"Standard_D4s_v5\"},\"timeCreated\":"
               "\"2026-08-03T10:00:00Z\",\"provisioningState\":" +
               cloud::gcp::detail::json_quote(state) + "}}";
    }

    reply route(std::string_view method, std::string_view target, std::string_view headers,
                std::string_view body) {
        if (method == "GET" && starts_with(target, "/transport-body?")) {
            const std::size_t bytes = static_cast<std::size_t>(std::stoull(query(target, "bytes")));
            const std::string status = query(target, "status");
            return {status.empty() ? 200 : std::stoi(status), std::string(bytes, 'x')};
        }
        if (method == "GET" && target == "/transport-headers") {
            reply response;
            for (int index = 0; index < 300; ++index)
                response.headers.push_back("X-Limit-" + std::to_string(index) + ": " +
                                           std::string(1024, 'x'));
            return response;
        }
        if (method == "POST" && target == "/ec2/") {
            const std::string authorisation = request_header(headers, "authorization");
            if (!starts_with(authorisation, "AWS4-HMAC-SHA256") ||
                authorisation.find("/eu-west-1/ec2/aws4_request") == std::string::npos ||
                request_header(headers, "x-amz-date").empty())
                aws_compute_valid = false;

            if (body.find("Action=RunInstances") != std::string_view::npos) {
                const int attempt = ++aws_compute_create_attempts;
                const std::string token = query(body, "ClientToken");
                if (token.empty() ||
                    body.find("MinCount=1&MaxCount=1") == std::string_view::npos ||
                    body.find("LaunchTemplate.LaunchTemplateId=lt-worker") ==
                        std::string_view::npos ||
                    body.find("LaunchTemplate.Version=7") == std::string_view::npos ||
                    body.find("TagSpecification.1.Tag.1.Key=Name") == std::string_view::npos ||
                    body.find("TagSpecification.1.Tag.1.Value=aws-created") ==
                        std::string_view::npos ||
                    body.find("TagSpecification.1.Tag.2.Key=cloud-hpp") ==
                        std::string_view::npos)
                    aws_compute_valid = false;
                if (attempt == 1) {
                    aws_compute_client_token_ = token;
                    return {500, "<Error><Message>retry</Message></Error>"};
                }
                if (token != aws_compute_client_token_)
                    aws_compute_valid = false;
                return {200,
                        "<RunInstancesResponse><instancesSet>" +
                            aws_instance_xml("i-created", "aws-created", "pending") +
                            "</instancesSet></RunInstancesResponse>"};
            }

            if (body.find("Action=DescribeInstances") != std::string_view::npos) {
                if (body.find("Filter.1.Name=tag%3AName") != std::string_view::npos) {
                    if (body.find("Filter.2.Name=tag%3Acloud-hpp&"
                                  "Filter.2.Value.1=managed") == std::string_view::npos)
                        aws_compute_valid = false;
                    if (body.find("Filter.1.Value.1=aws-start") != std::string_view::npos)
                        return {200, aws_instances_xml(
                                         aws_instance_xml("i-start", "aws-start", "stopped"))};
                    if (body.find("Filter.1.Value.1=i-worker") != std::string_view::npos)
                        return {200, aws_instances_xml(
                                         aws_instance_xml("i-0123456789abcdef0", "i-worker",
                                                          "stopped"))};
                    if (body.find("Filter.1.Value.1=aws-stop") != std::string_view::npos)
                        return {200, aws_instances_xml(
                                         aws_instance_xml("i-stop", "aws-stop", "running"))};
                    if (body.find("Filter.1.Value.1=aws-destroy") != std::string_view::npos)
                        return {200,
                                aws_instances_xml(aws_instance_xml(
                                    "i-destroy", "aws-destroy", "running"))};
                    if (body.find("Filter.1.Value.1=aws-existing") != std::string_view::npos)
                        return body.find("NextToken=name-existing-2") != std::string_view::npos
                                   ? reply{200,
                                           aws_instances_xml(aws_instance_xml(
                                               "i-existing", "aws-existing", "running"))}
                                   : reply{200, aws_instances_xml({}, "name-existing-2")};
                    if (body.find("Filter.1.Value.1=aws-ambiguous") != std::string_view::npos)
                        return body.find("NextToken=name-ambiguous-2") != std::string_view::npos
                                   ? reply{200,
                                           aws_instances_xml(aws_instance_xml(
                                               "i-ambiguous-b", "aws-ambiguous", "stopped"))}
                                   : reply{200,
                                           aws_instances_xml(
                                               aws_instance_xml("i-ambiguous-a", "aws-ambiguous",
                                                                "stopped"),
                                               "name-ambiguous-2")};
                    return {200, aws_instances_xml({})};
                }
                if (body.find("InstanceId.1=i-created") != std::string_view::npos) {
                    if (aws_compute_retry_wait)
                        return {500, "<Error><Message>retry wait</Message></Error>"};
                    const int read = ++aws_compute_create_polls;
                    return {200,
                            aws_instances_xml(aws_instance_xml(
                                "i-created", "aws-created", read == 1 ? "pending" : "running"))};
                }
                if (body.find("InstanceId.1=i-start") != std::string_view::npos) {
                    const int read = ++aws_compute_start_polls;
                    return {200,
                            aws_instances_xml(aws_instance_xml(
                                "i-start", "aws-start", read == 1 ? "pending" : "running"))};
                }
                if (body.find("InstanceId.1=i-0123456789abcdef0") != std::string_view::npos) {
                    ++aws_compute_start_polls;
                    return {200,
                            aws_instances_xml(aws_instance_xml(
                                "i-0123456789abcdef0", "i-worker", "running"))};
                }
                if (body.find("InstanceId.1=i-stop") != std::string_view::npos) {
                    const int read = ++aws_compute_stop_polls;
                    return {200,
                            aws_instances_xml(aws_instance_xml(
                                "i-stop", "aws-stop", read == 1 ? "stopping" : "stopped"))};
                }
                if (body.find("InstanceId.1=i-destroy") != std::string_view::npos) {
                    const int read = ++aws_compute_destroy_polls;
                    return {200, aws_instances_xml(aws_instance_xml(
                                     "i-destroy", "aws-destroy",
                                     read == 1 ? "shutting-down" : "terminated"))};
                }
                ++aws_compute_lists;
                if (body.find("NextToken=page-2") != std::string_view::npos)
                    return {200,
                            aws_instances_xml(
                                aws_instance_xml("i-list-b", "list-b", "running"))};
                return {200,
                        aws_instances_xml(aws_instance_xml("i-list-a", "list-a", "stopped"),
                                          "page-2")};
            }

            if (body.find("Action=StartInstances") != std::string_view::npos) {
                ++aws_compute_starts;
                if (body.find("InstanceId.1=i-start") == std::string_view::npos &&
                    body.find("InstanceId.1=i-0123456789abcdef0") == std::string_view::npos)
                    aws_compute_valid = false;
                return {200, "<StartInstancesResponse/>"};
            }
            if (body.find("Action=StopInstances") != std::string_view::npos) {
                ++aws_compute_stops;
                if (body.find("InstanceId.1=i-stop") == std::string_view::npos)
                    aws_compute_valid = false;
                return {200, "<StopInstancesResponse/>"};
            }
            if (body.find("Action=TerminateInstances") != std::string_view::npos) {
                ++aws_compute_destroys;
                if (body.find("InstanceId.1=i-destroy") == std::string_view::npos)
                    aws_compute_valid = false;
                return {200, "<TerminateInstancesResponse/>"};
            }
            aws_compute_valid = false;
            return {400, "<Error><Message>unknown EC2 action</Message></Error>"};
        }

        if (starts_with(target, "/arm/")) {
            if (!has_header(headers, "authorization", "Bearer azure-management-token") ||
                !request_header(headers, "ocp-date").empty())
                azure_compute_valid = false;

            constexpr std::string_view collection =
                "/arm/subscriptions/subscription/resourceGroups/workers/providers/"
                "Microsoft.Compute/virtualMachines";
            if (method == "GET" && starts_with(target, collection) &&
                target.find("/instanceView?") == std::string_view::npos) {
                ++azure_compute_lists;
                const std::string next = azure_cross_origin_next
                                             ? "https://evil.example/virtualMachines?page=2"
                                             : url() + "/arm/pages/vms-2?api-version=2025-04-01";
                return {200, "{\"value\":[" + azure_vm_json("azure-list-a", "Succeeded") +
                                 "],\"nextLink\":" +
                                 cloud::gcp::detail::json_quote(next) + "}"};
            }
            if (method == "GET" && starts_with(target, "/arm/pages/vms-2?")) {
                ++azure_compute_lists;
                return {200, "{\"value\":[" +
                                 azure_vm_json("azure-list-b", "Succeeded") + "]}"};
            }
            if (method == "GET" && target.find("/instanceView?") != std::string_view::npos) {
                std::string state = "running";
                if (target.find("azure-list-a") != std::string_view::npos)
                    state = "stopped";
                else if (target.find("azure-stop") != std::string_view::npos)
                    state = "deallocated";
                return {200, "{\"statuses\":[{\"code\":\"PowerState/" + state + "\"}]}"};
            }
            if (method == "PUT" && starts_with(target, collection) &&
                target.find("/azure-") != std::string_view::npos) {
                ++azure_compute_creates;
                try {
                    (void)cloud::gcp::detail::parse_json(body);
                } catch (const cloud::error&) {
                    azure_compute_valid = false;
                }
                if (!has_header(headers, "if-none-match", "*") ||
                    body.find("\"location\":\"uksouth\"") == std::string_view::npos ||
                    body.find("\"vmSize\":\"Standard_D4s_v5\"") ==
                        std::string_view::npos ||
                    body.find("\"imageReference\":{\"id\":\"/images/worker\"}") ==
                        std::string_view::npos ||
                    body.find("\"osDisk\":{\"name\":\"azure-") ==
                        std::string_view::npos ||
                    body.find("\"createOption\":\"FromImage\",\"deleteOption\":\"Delete\"") ==
                        std::string_view::npos ||
                    body.find("\"networkInterfaceConfigurations\":[{") ==
                        std::string_view::npos ||
                    body.find("\"primary\":true,\"deleteOption\":\"Delete\"") ==
                        std::string_view::npos ||
                    body.find("\"subnet\":{\"id\":\"/networks/subnets/workers\"}") ==
                        std::string_view::npos)
                    azure_compute_valid = false;
                const std::string poll = azure_cross_origin_operation
                                             ? "https://evil.example/operations/create"
                                             : url() + "/arm/operations/create";
                return {202, "{}", {"Azure-AsyncOperation: " + poll}};
            }
            if (method == "POST" && target.find("/azure-start/start?") !=
                                        std::string_view::npos) {
                ++azure_compute_starts;
                return {202, "{}", {"Location: " + url() + "/arm/operations/start"}};
            }
            if (method == "POST" && target.find("/azure-stop/deallocate?") !=
                                        std::string_view::npos) {
                ++azure_compute_deallocates;
                return {202, "{}", {"Azure-AsyncOperation: " +
                                      url() + "/arm/operations/deallocate"}};
            }
            if (method == "DELETE" && target.find("/azure-destroy?") !=
                                          std::string_view::npos) {
                ++azure_compute_deletes;
                return {202, "{}", {"Azure-AsyncOperation: " +
                                      url() + "/arm/operations/delete"}};
            }
            if (method == "GET" && starts_with(target, "/arm/operations/")) {
                if (azure_compute_retry_wait)
                    return {500, "{\"error\":{\"message\":\"retry wait\"}}"};
                const int read = ++azure_compute_operation_polls;
                if (target == "/arm/operations/create" && read == 1)
                    return {200, "{\"status\":\"InProgress\"}"};
                return {200, "{\"status\":\"Succeeded\"}"};
            }
            azure_compute_valid = false;
            return {404, "{\"error\":{\"message\":\"unknown ARM request\"}}"};
        }

        if (method == "POST" && target == "/v1/registerjobdefinition") {
            ++aws_registers;
            const bool fargate =
                body.find("\"platformCapabilities\":[\"FARGATE\"]") != std::string_view::npos;
            try {
                (void)cloud::gcp::detail::parse_json(body);
            } catch (const cloud::error&) {
                if (fargate)
                    aws_mount_body_valid = false;
                else
                    aws_body_valid = false;
            }
            if (fargate) {
                ++aws_fargate_registers;
                if (body.find("\"type\":\"VCPU\",\"value\":\"2\"") ==
                        std::string_view::npos ||
                    body.find("\"type\":\"MEMORY\",\"value\":\"4096\"") ==
                        std::string_view::npos ||
                    body.find("\"type\":\"GPU\"") != std::string_view::npos ||
                    body.find("\"jobRoleArn\":\"arn:aws:iam::1:role/job\"") ==
                        std::string_view::npos ||
                    body.find("\"executionRoleArn\":\"arn:aws:iam::1:role/execution\"") ==
                        std::string_view::npos ||
                    body.find("\"fargatePlatformConfiguration\":{\"platformVersion\":"
                              "\"LATEST\"}") == std::string_view::npos ||
                    body.find("\"fileSystemArn\":\"arn:aws:s3files:eu-west-1:1:"
                              "file-system/fs-test\"") == std::string_view::npos ||
                    body.find("\"rootDirectory\":\"/prefix/\"") == std::string_view::npos ||
                    body.find("\"containerPath\":\"/inputs\",\"readOnly\":true") ==
                        std::string_view::npos)
                    aws_mount_body_valid = false;
            } else if (body.find("\"platformCapabilities\":[\"EC2\"]") ==
                               std::string_view::npos ||
                       body.find("\"type\":\"VCPU\",\"value\":\"4\"") ==
                           std::string_view::npos ||
                       body.find("\"type\":\"GPU\",\"value\":\"1\"") ==
                           std::string_view::npos) {
                aws_body_valid = false;
            }
            if (aws_ambiguous_register)
                return {0, {}};
            return {200,
                    "{\"jobDefinitionArn\":\"arn:aws:batch:eu-west-1:1:job-definition/cloud:1\"}"};
        }
        if (method == "POST" && target == "/v1/describejobdefinitions") {
            ++aws_definition_reads;
            std::string name;
            try {
                const auto json = cloud::gcp::detail::parse_json(body);
                name = cloud::gcp::detail::field(json, "jobDefinitionName");
            } catch (const cloud::error&) {
                aws_body_valid = false;
            }
            if (name.empty() || body.find("\"status\":\"ACTIVE\"") == std::string_view::npos)
                aws_body_valid = false;
            return {
                200,
                "{\"jobDefinitions\":[{\"jobDefinitionName\":" +
                    cloud::gcp::detail::json_quote(name) +
                    ",\"jobDefinitionArn\":\"arn:aws:batch:eu-west-1:1:job-definition/cloud:1\","
                    "\"status\":\"ACTIVE\"}]}"};
        }
        if (method == "POST" && target == "/v1/submitjob") {
            ++aws_submits;
            const bool fargate = aws_fargate_registers.load() > aws_fargate_submits.load();
            try {
                (void)cloud::gcp::detail::parse_json(body);
            } catch (const cloud::error&) {
                if (fargate)
                    aws_mount_body_valid = false;
                else
                    aws_body_valid = false;
            }
            if (fargate) {
                ++aws_fargate_submits;
                if (body.find("\"jobQueue\":\"fargate-queue\"") == std::string_view::npos ||
                    body.find("\"tags\":{\"cloud-hpp\":\"temporary\"}") ==
                        std::string_view::npos)
                    aws_mount_body_valid = false;
            } else if (body.find("\"jobQueue\":\"gpu-queue\"") == std::string_view::npos ||
                       body.find("\"attempts\":2") == std::string_view::npos ||
                       body.find("\"tags\":{\"cloud-hpp\":\"temporary\"}") ==
                           std::string_view::npos) {
                aws_body_valid = false;
            }
            return {200, "{\"jobId\":\"aws-job-id\",\"jobName\":\"cloud-job\"}"};
        }
        if (method == "POST" && target == "/v1/describejobs" && aws_cancel_mode != 0) {
            if (aws_cancel_accepted)
                return {200,
                        "{\"jobs\":[{\"jobId\":\"aws-job-id\",\"status\":\"FAILED\","
                        "\"statusReason\":\"cloud cancellation\","
                        "\"container\":{\"exitCode\":1,\"logStreamName\":\"cloud/stream\"}}]}"};
            const std::string state = aws_cancel_mode == 1 ? "RUNNABLE" : "STARTING";
            return {200, "{\"jobs\":[{\"jobId\":\"aws-job-id\",\"status\":" +
                             cloud::gcp::detail::json_quote(state) +
                             ",\"container\":{\"logStreamName\":\"cloud/stream\"}}]}"};
        }
        if (method == "POST" && target == "/v1/describejobs")
            return {200, "{\"jobs\":[{\"jobId\":\"aws-job-id\",\"status\":\"SUCCEEDED\","
                         "\"container\":{\"exitCode\":0,\"logStreamName\":\"cloud/stream\"}}]}"};
        if (method == "POST" && ((target == "/v1/canceljob" && aws_cancel_mode == 1) ||
                                 (target == "/v1/terminatejob" && aws_cancel_mode == 2))) {
            const int request = ++aws_cancel_requests;
            if (request == 1)
                return {0, {}};
            aws_cancel_accepted = true;
            return {};
        }
        if (method == "POST" && target == "/v1/deregisterjobdefinition") {
            ++aws_deregisters;
            return {};
        }
        if (method == "POST" && target == "/" &&
            body.find("\"logGroupName\"") != std::string_view::npos) {
            const int read = ++aws_log_reads;
            if (aws_endless_logs)
                return {200, "{\"events\":[],\"nextForwardToken\":\"page-" + std::to_string(read) +
                                 "\"}"};
            if (body.find("\"nextToken\":\"page-") != std::string_view::npos)
                return {400, "{\"message\":\"stale speculative cursor\"}"};
            if (body.find("\"nextToken\":\"done\"") != std::string_view::npos)
                return {200, "{\"events\":[],\"nextForwardToken\":\"done\"}"};
            return {200, "{\"events\":[{\"timestamp\":\"1000\",\"ingestionTime\":\"1001\","
                         "\"message\":\"aws-out\"}],\"nextForwardToken\":\"done\"}"};
        }
        if (method == "POST" && target == "/" &&
            body.find("\"ServiceCode\":\"AmazonECS\"") != std::string_view::npos) {
            ++aws_fargate_price_reads;
            if (body.find("\"Field\":\"regionCode\",\"Value\":\"eu-west-1\"") ==
                    std::string_view::npos ||
                body.find("\"Field\":\"tenancy\",\"Value\":\"Shared\"") == std::string_view::npos ||
                body.find("operatingSystem") != std::string_view::npos)
                aws_fargate_price_valid = false;
            if (body.find("\"NextToken\":\"fargate-page-2\"") != std::string_view::npos)
                return {200, "{\"PriceList\":[" +
                                 aws_fargate_product("EU-Fargate-GB-Hours", "memorytype", "perGB",
                                                     "0.0050000000") +
                                 "]}"};
            return {200,
                    "{\"PriceList\":[" +
                        aws_fargate_product("EU-Fargate-vCPU-Hours:perCPU", "cputype", "perCPU",
                                            "0.0400000000") +
                        ',' +
                        aws_fargate_product("EU-Fargate-ARM-vCPU-Hours:perCPU", "cputype", "perCPU",
                                            "9.0000000000", ",\"cpuArchitecture\":\"ARM\"") +
                        ',' +
                        aws_fargate_product("EU-Fargate-Windows-GB-Hours", "memorytype", "perGB",
                                            "8.0000000000") +
                        "],\"NextToken\":\"fargate-page-2\"}"};
        }
        if (method == "POST" && target == "/" &&
            body.find("\"ServiceCode\":\"AmazonEC2\"") != std::string_view::npos) {
            ++aws_price_reads;
            return {200, "{\"PriceList\":[\"{\\\"terms\\\":{\\\"OnDemand\\\":{\\\"term\\\":{"
                         "\\\"priceDimensions\\\":{\\\"dimension\\\":{\\\"unit\\\":\\\"Hrs\\\","
                         "\\\"pricePerUnit\\\":{\\\"USD\\\":\\\"0.4200000000\\\"}}}}}}}\"]}"};
        }
        if (method == "POST" && target == "/" &&
            body.find("DescribeSpotPriceHistory") != std::string_view::npos) {
            ++aws_spot_reads;
            return {200, "<DescribeSpotPriceHistoryResponse><spotPriceHistorySet><item>"
                         "<spotPrice>0.125</spotPrice></item></spotPriceHistorySet>"
                         "</DescribeSpotPriceHistoryResponse>"};
        }
        if (method == "GET" && starts_with(target, "/v1/services/6F81-5844-456A/skus")) {
            ++gcp_price_reads;
            if (target.find("pageSize=200") == std::string_view::npos)
                return {400, "{\"error\":\"unsafe page size\"}"};
            if (target.find("pageToken=page-2") != std::string_view::npos)
                return {200,
                        "{\"skus\":[" +
                            gcp_sku("G2 Instance Core", "OnDemand", 20'000'000) + ',' +
                            gcp_sku("G2 Custom Instance Core", "OnDemand", 21'000'000) + ',' +
                            gcp_sku("G2 Sole Tenancy Instance Core", "OnDemand", 22'000'000) +
                            ',' + gcp_sku("G2 Instance Ram", "OnDemand", 3'000'000) + ',' +
                            gcp_sku("G2 Custom Instance Ram", "OnDemand", 4'000'000) + ',' +
                            gcp_sku("G2 Sole Tenancy Instance Ram", "OnDemand", 5'000'000) + ',' +
                            gcp_sku("Nvidia L4 GPU", "OnDemand", 500'000'000) + ',' +
                            gcp_sku("Nvidia L4 GPU", "Commit1Yr", 400'000'000) + "]}"};
            return {200, "{\"skus\":[" + gcp_sku("E2 Instance Core", "OnDemand", 10'000'000) + ',' +
                             gcp_sku("E2 Custom Instance Core", "OnDemand", 11'000'000) + ',' +
                             gcp_sku("E2 Instance Ram", "OnDemand", 2'000'000) + ',' +
                             gcp_sku("E2 Custom Instance Ram", "OnDemand", 3'000'000) + ',' +
                             gcp_sku("E2 Instance Core", "Commit1Yr", 4'000'000) +
                             "],\"nextPageToken\":\"page-2\"}"};
        }
        if (method == "GET" && (target.find("%24filter=") != std::string_view::npos ||
                                target.find("$filter=") != std::string_view::npos)) {
            ++azure_price_reads;
            return {
                200,
                "{\"Items\":[{\"currencyCode\":\"USD\",\"armRegionName\":\"westeurope\","
                "\"armSkuName\":\"Standard_D4s_v5\",\"type\":\"Consumption\","
                "\"unitOfMeasure\":\"1 Hour\",\"isPrimaryMeterRegion\":true,"
                "\"productName\":\"Virtual Machines Dsv5 Series\","
                "\"meterName\":\"D4s v5\",\"skuName\":\"D4s v5\","
                "\"effectiveStartDate\":\"2020-01-01T00:00:00Z\",\"retailPrice\":0.30},"
                "{\"currencyCode\":\"USD\",\"armRegionName\":\"westeurope\","
                "\"armSkuName\":\"Standard_D4s_v5\",\"type\":\"Consumption\","
                "\"unitOfMeasure\":\"1 Hour\",\"isPrimaryMeterRegion\":true,"
                "\"productName\":\"Virtual Machines Dsv5 Series\","
                "\"meterName\":\"D4s v5\",\"skuName\":\"D4s v5\","
                "\"effectiveStartDate\":\"2024-01-01T00:00:00Z\",\"retailPrice\":0.20},"
                "{\"currencyCode\":\"USD\",\"armRegionName\":\"westeurope\","
                "\"armSkuName\":\"Standard_D4s_v5\",\"type\":\"Consumption\","
                "\"unitOfMeasure\":\"1 Hour\",\"isPrimaryMeterRegion\":true,"
                "\"productName\":\"Virtual Machines Dsv5 Series\","
                "\"meterName\":\"D4s v5\",\"skuName\":\"D4s v5\","
                "\"effectiveStartDate\":\"9999-01-01T00:00:00Z\",\"retailPrice\":0.10},"
                "{\"currencyCode\":\"USD\","
                "\"armRegionName\":\"westeurope\","
                "\"armSkuName\":\"Standard_D4s_v5 Spot\","
                "\"type\":\"Consumption\",\"unitOfMeasure\":\"1 Hour\","
                "\"isPrimaryMeterRegion\":true,\"productName\":\"Virtual Machines Dsv5 Series\","
                "\"meterName\":\"D4s v5 Spot\",\"skuName\":\"D4s v5 Spot\","
                "\"effectiveStartDate\":\"2024-01-01T00:00:00Z\",\"retailPrice\":0.04},"
                "{\"currencyCode\":\"USD\","
                "\"armRegionName\":\"westeurope\",\"armSkuName\":\"Standard_D4s_v5\","
                "\"type\":\"Consumption\",\"unitOfMeasure\":\"1 Hour\","
                "\"isPrimaryMeterRegion\":true,\"productName\":\"Virtual Machines Dsv5 Series\","
                "\"meterName\":\"D4s v5 Low Priority\",\"skuName\":\"D4s v5 Low Priority\","
                "\"effectiveStartDate\":\"2024-01-01T00:00:00Z\","
                "\"retailPrice\":0.05}],\"NextPageLink\":" +
                    cloud::gcp::detail::json_quote(
                        azure_cross_origin_price ? "https://evil.example/prices?page=2" : "") +
                    "}"};
        }

        if (method == "POST" && starts_with(target, "/jobs?api-version=")) {
            const int writes = ++azure_jobs;
            const bool mounted = body.find("\"mountConfiguration\"") != std::string_view::npos;
            try {
                (void)cloud::gcp::detail::parse_json(body);
            } catch (const cloud::error&) {
                if (mounted)
                    azure_mount_body_valid = false;
                else
                    azure_body_valid = false;
            }
            if (mounted) {
                ++azure_mount_jobs;
                if (body.find("\"accountName\":\"storageaccount\"") ==
                        std::string_view::npos ||
                    body.find("\"containerName\":\"input-container\"") ==
                        std::string_view::npos ||
                    body.find("\"relativeMountPath\":\"cloud-0\"") ==
                        std::string_view::npos ||
                    body.find("\"sasKey\":\"sv=test&sig=secret\"") ==
                        std::string_view::npos ||
                    body.find("\"blobfuseOptions\":\"-o ro\"") == std::string_view::npos)
                    azure_mount_body_valid = false;
            } else if (body.find("\"vmSize\":\"Standard_NC24ads_A100_v4\"") ==
                               std::string_view::npos ||
                       body.find("\"targetLowPriorityNodes\":1") == std::string_view::npos) {
                azure_body_valid = false;
            }
            if (writes == 1)
                return {500, "{\"code\":\"ambiguous\"}"};
            return {};
        }
        if (method == "POST" && target.find("/tasks?api-version=") != std::string_view::npos) {
            const int writes = ++azure_tasks;
            const bool mounted =
                body.find("/mnt/batch/tasks/fsmounts/cloud-0") != std::string_view::npos;
            try {
                (void)cloud::gcp::detail::parse_json(body);
            } catch (const cloud::error&) {
                if (mounted)
                    azure_mount_body_valid = false;
                else
                    azure_body_valid = false;
            }
            if (mounted) {
                ++azure_mount_tasks;
                if (body.find("--volume=/mnt/batch/tasks/fsmounts/cloud-0:/inputs:ro") ==
                        std::string_view::npos ||
                    body.find("\"userIdentity\":{\"autoUser\":{\"scope\":\"task\","
                              "\"elevationLevel\":\"admin\"}}") == std::string_view::npos)
                    azure_mount_body_valid = false;
            } else if (body.find("\"commandLine\":\"run --fast\"") ==
                               std::string_view::npos ||
                       body.find("\"imageName\":\"image\"") == std::string_view::npos) {
                azure_body_valid = false;
            }
            if (writes == 1)
                return {500, "{\"code\":\"ambiguous\"}"};
            return {};
        }
        if (method == "HEAD" &&
            target.find("/tasks/task/files/stdout.txt?") != std::string_view::npos)
            return azure_task_reads < 3   ? reply{200, "attempt-two"}
                   : azure_task_reads < 5 ? reply{200, "attempt-restart"}
                                          : reply{200, "attempt-ten\n"};
        if (method == "HEAD" &&
            target.find("/tasks/task/files/stderr.txt?") != std::string_view::npos)
            return {200, ""};
        if (method == "GET" &&
            target.find("/tasks/task/files/stdout.txt?") != std::string_view::npos)
            return azure_task_reads < 3   ? reply{200, "attempt-two"}
                   : azure_task_reads < 5 ? reply{200, "attempt-restart"}
                                          : reply{200, "attempt-ten\n"};
        if (method == "GET" &&
            target.find("/tasks/task/files/stderr.txt?") != std::string_view::npos)
            return {200, ""};
        if (method == "GET" && target.find("/tasks/task?api-version=") != std::string_view::npos) {
            const int read = azure_task_reads++;
            if (read < 2)
                return {200, "{\"state\":\"running\",\"executionInfo\":{"
                             "\"startTime\":\"2026-08-03T10:00:00Z\",\"retryCount\":2,"
                             "\"requeueCount\":0}}"};
            if (read < 4)
                return {200, "{\"state\":\"running\",\"executionInfo\":{"
                             "\"startTime\":\"2026-08-03T10:01:00Z\",\"retryCount\":2,"
                             "\"requeueCount\":0}}"};
            return {200, "{\"state\":\"completed\",\"executionInfo\":{\"result\":\"success\","
                         "\"startTime\":\"2026-08-03T10:02:00Z\",\"retryCount\":10,"
                         "\"requeueCount\":0,\"exitCode\":0}}"};
        }
        if (method == "POST" && target.find("/terminate?api-version=") != std::string_view::npos &&
            target.find("/tasks/") == std::string_view::npos) {
            ++azure_terminates;
            return {};
        }
        if (method == "GET" && starts_with(target, "/jobs/") &&
            target.find("/tasks/") == std::string_view::npos &&
            target.find("api-version=") != std::string_view::npos) {
            if (azure_deletes == 0)
                return azure_terminates == 0 ? reply{200, "{\"state\":\"active\"}"}
                                             : reply{200, "{\"state\":\"completed\"}"};
            if (azure_delete_polls++ == 0)
                return {200, "{\"state\":\"deleting\"}"};
            return {404, "{\"code\":\"JobNotFound\"}"};
        }
        if (method == "DELETE" && starts_with(target, "/jobs/") &&
            target.find("api-version=") != std::string_view::npos) {
            ++azure_deletes;
            return {202, "{}"};
        }

        // The provider-neutral storage facade deliberately uses path-style
        // endpoints for explicit test servers. This keeps the tests local while
        // still exercising AWS SigV4 and Azure's storage-scoped bearer headers.
        constexpr std::string_view aws_bucket = "/s3/test-bucket";
        if (starts_with(target, aws_bucket)) {
            const auto query_begin = target.find('?');
            const auto path = target.substr(0, query_begin);
            const bool authenticated = starts_with(request_header(headers, "authorization"),
                                                   "AWS4-HMAC-SHA256") &&
                                       !request_header(headers, "x-amz-date").empty();
            if (!authenticated)
                aws_storage_valid = false;

            if (path == aws_bucket && method == "GET") {
                const int page = ++aws_storage_lists;
                if (query(target, "prefix") != "dir%2F" ||
                    query(target, "delimiter") != "%2F")
                    aws_storage_valid = false;
                if (page == 1)
                    return {200,
                            "<ListBucketResult><Contents><Key>dir%2Ffirst%20file</Key>"
                            "<ETag>&quot;aws-list-1&quot;</ETag><Size>7</Size>"
                            "<LastModified>2026-08-03T10:00:00Z</LastModified></Contents>"
                            "<CommonPrefixes><Prefix>dir%2Fsub%2F</Prefix></CommonPrefixes>"
                            "<IsTruncated>true</IsTruncated>"
                            "<NextContinuationToken>next</NextContinuationToken>"
                            "</ListBucketResult>"};
                if (query(target, "continuation-token") != "next")
                    aws_storage_valid = false;
                return {200,
                        "<ListBucketResult><Contents><Key>dir%2Fsecond</Key>"
                        "<ETag>&quot;aws-list-2&quot;</ETag><Size>4</Size>"
                        "<LastModified>2026-08-03T10:01:00Z</LastModified></Contents>"
                        "<IsTruncated>false</IsTruncated></ListBucketResult>"};
            }

            const std::string key(path.size() > aws_bucket.size() + 1
                                      ? path.substr(aws_bucket.size() + 1)
                                      : std::string_view{});
            if (key.empty())
                return {400, "missing S3 object key"};
            if (method == "PUT") {
                const std::string checksum = cloud::gcp::detail::crc32c(body);
                const bool file = key == "file";
                if (file)
                    ++aws_storage_file_puts;
                else
                    ++aws_storage_puts;
                if (!has_header(headers, "x-amz-checksum-crc32c", checksum) ||
                    !has_header(headers, "x-amz-meta-cloud-crc32c", checksum) ||
                    (body.empty() && !has_header(headers, "content-length", "0")) ||
                    (file && !has_header(headers, "if-match", "\"aws-version\"")) ||
                    (!file && !has_header(headers, "if-none-match", "*")))
                    aws_storage_valid = false;
                aws_objects_[key] = std::string(body);
                return {};
            }
            const auto found = aws_objects_.find(key);
            if (found == aws_objects_.end())
                return {404, "missing S3 object"};
            const std::string checksum = cloud::gcp::detail::crc32c(found->second);
            if (method == "HEAD") {
                return {200,
                        "",
                        {"ETag: \"aws-version\"",
                         "Content-Length: " + std::to_string(found->second.size()),
                         "Content-Type: application/octet-stream",
                         "Last-Modified: Mon, 03 Aug 2026 10:00:00 GMT",
                         "x-amz-meta-cloud-crc32c: " + checksum}};
            }
            if (method == "GET") {
                ++aws_storage_gets;
                if (!has_header(headers, "if-match", "\"aws-version\""))
                    aws_storage_valid = false;
                return {200, corrupt_download ? "corrupt" : found->second};
            }
            if (method == "DELETE") {
                ++aws_storage_deletes;
                aws_objects_.erase(found);
                return {204, ""};
            }
        }

        constexpr std::string_view azure_container = "/blob/test-container";
        if (starts_with(target, azure_container)) {
            const auto query_begin = target.find('?');
            const auto path = target.substr(0, query_begin);
            const bool authenticated =
                has_header(headers, "authorization", "Bearer azure-storage-token") &&
                has_header(headers, "x-ms-version", "2023-11-03") &&
                !request_header(headers, "x-ms-date").empty();
            if (!authenticated)
                azure_storage_valid = false;

            if (path == azure_container && method == "GET") {
                const int page = ++azure_storage_lists;
                if (query(target, "prefix") != "dir%2F" ||
                    query(target, "delimiter") != "%2F")
                    azure_storage_valid = false;
                if (page == 1)
                    return {200,
                            "<EnumerationResults><Blobs><Blob><Name Encoded=\"true\">"
                            "dir%2Ffirst%20%26%20data</Name>"
                            "<Properties><Etag>&quot;azure-list-1&quot;</Etag>"
                            "<Content-Length>7</Content-Length><Content-Type>text/plain</Content-Type>"
                            "<Last-Modified>Mon, 03 Aug 2026 10:00:00 GMT</Last-Modified>"
                            "</Properties><Metadata><cloudcrc32c>AAAAAA==</cloudcrc32c>"
                            "</Metadata></Blob><BlobPrefix><Name Encoded=\"true\">"
                            "dir%2Fsub%2F</Name></BlobPrefix>"
                            "</Blobs><NextMarker>next</NextMarker></EnumerationResults>"};
                if (query(target, "marker") != "next")
                    azure_storage_valid = false;
                return {200,
                        "<EnumerationResults><Blobs><Blob><Name>dir/second</Name>"
                        "<Properties><Etag>&quot;azure-list-2&quot;</Etag>"
                        "<Content-Length>4</Content-Length></Properties>"
                        "</Blob></Blobs><NextMarker></NextMarker></EnumerationResults>"};
            }

            const std::string key(path.size() > azure_container.size() + 1
                                      ? path.substr(azure_container.size() + 1)
                                      : std::string_view{});
            if (key.empty())
                return {400, "missing Azure Blob key"};
            if (method == "PUT") {
                const std::string checksum = cloud::gcp::detail::crc32c(body);
                const bool file = key == "file";
                if (file)
                    ++azure_storage_file_puts;
                else
                    ++azure_storage_puts;
                if (!has_header(headers, "x-ms-blob-type", "BlockBlob") ||
                    !has_header(headers, "x-ms-meta-cloudcrc32c", checksum) ||
                    (body.empty() && !has_header(headers, "content-length", "0")) ||
                    (file && !has_header(headers, "if-match", "\"azure-version\"")) ||
                    (!file && !has_header(headers, "if-none-match", "*")))
                    azure_storage_valid = false;
                azure_objects_[key] = std::string(body);
                return {201, ""};
            }
            const auto found = azure_objects_.find(key);
            if (found == azure_objects_.end())
                return {404, "missing Azure Blob"};
            const std::string checksum = cloud::gcp::detail::crc32c(found->second);
            if (method == "HEAD")
                return {200,
                        "",
                        {"ETag: \"azure-version\"",
                         "Content-Length: " + std::to_string(found->second.size()),
                         "Content-Type: application/octet-stream",
                         "Last-Modified: Mon, 03 Aug 2026 10:00:00 GMT",
                         "x-ms-meta-cloudcrc32c: " + checksum}};
            if (method == "GET") {
                ++azure_storage_gets;
                if (!has_header(headers, "if-match", "\"azure-version\""))
                    azure_storage_valid = false;
                return {200, corrupt_download ? "corrupt" : found->second};
            }
            if (method == "DELETE") {
                ++azure_storage_deletes;
                azure_objects_.erase(found);
                return {202, ""};
            }
        }

        constexpr std::string_view object_path = "/storage/v1/b/test-bucket/o/object";
        constexpr std::string_view download_path = "/download/storage/v1/b/test-bucket/o/object";
        if (method == "POST" && starts_with(target, "/upload/storage/v1/b/test-bucket/o?")) {
            if (query(target, "name") != "object")
                return {400, "missing object name"};
            return {200, "{\"name\":\"object\",\"generation\":\"7\","
                         "\"size\":\"" +
                             std::to_string(body.size()) + "\",\"crc32c\":\"" +
                             cloud::gcp::detail::crc32c(body) + "\"}"};
        }
        if (method == "GET" && starts_with(target, download_path)) {
            if (query(target, "generation") != "7")
                return {400, "missing generation"};
            return {200, corrupt_download ? "corrupt" : "payload"};
        }
        if (method == "GET" && starts_with(target, object_path)) {
            return {200, "{\"name\":\"object\",\"generation\":\"7\","
                         "\"size\":\"7\",\"crc32c\":\"" +
                             cloud::gcp::detail::crc32c("payload") + "\"}"};
        }
        if (method == "GET" && starts_with(target, "/storage/v1/b/test-bucket/o?")) {
            ++storage_lists;
            if (query(target, "pageToken") == "next")
                return {200, "{\"items\":[{\"name\":\"second\"}]}"};
            return {200, "{\"items\":[{\"name\":\"first\"}],"
                         "\"prefixes\":[\"dir/\"],\"nextPageToken\":\"next\"}"};
        }
        if (method == "DELETE" && starts_with(target, object_path))
            return {};

        if (method == "POST" && target.find("/instances?sourceInstanceTemplate=") !=
                                    std::string_view::npos) {
            ++gcp_compute_creates;
            if (!has_header(headers, "authorization", "Bearer test") ||
                query(target, "sourceInstanceTemplate") !=
                    "global%2FinstanceTemplates%2Fgcp-worker-template" ||
                query(target, "requestId").empty())
                gcp_compute_valid = false;
            try {
                const auto json = cloud::gcp::detail::parse_json(body);
                if (cloud::gcp::detail::field(json, "name") != "gcp-created")
                    gcp_compute_valid = false;
            } catch (const cloud::error&) {
                gcp_compute_valid = false;
            }
            return {200, "{\"name\":\"insert-gcp-created\",\"status\":\"PENDING\"}"};
        }

        if (method == "GET" && target.find("/instances?") != std::string_view::npos) {
            if (query(target, "pageToken") == "next")
                return {200, "{\"items\":[{\"name\":\"vm-b\",\"status\":\"RUNNING\"}]}"};
            return {200, "{\"items\":[{\"name\":\"vm-a\",\"status\":\"STOPPED\"}],"
                         "\"nextPageToken\":\"next\"}"};
        }

        if (method == "POST" && target.find("/jobs?") != std::string_view::npos) {
            const std::string id = query(target, "jobId");
            const std::string request = query(target, "requestId");
            if (auto [it, inserted] = request_ids_.emplace(id, request);
                !inserted && it->second != request)
                request_id_changed = true;
            const int attempt = create_attempts_[id]++;
            if (id.find("retry-success") != std::string::npos && attempt == 0) {
                ++create_retries;
                return {500, "{\"error\":{\"message\":\"retry\"}}"};
            }
            return {200, "{\"name\":\"projects/test-project/locations/europe-west4/jobs/" + id +
                             "\",\"uid\":\"uid-" + id + "\",\"status\":{\"state\":\"QUEUED\"}}"};
        }

        if (method == "POST" && target.find("/v2/entries:list") != std::string_view::npos) {
            std::string id;
            for (const auto& [known, unused] : create_attempts_) {
                (void)unused;
                if (body.find("uid-" + known) != std::string_view::npos) {
                    id = known;
                    break;
                }
            }
            if (body.find("receiveTimestamp") != std::string_view::npos)
                ++cursor_queries;
            if (body.find("2026-01-01T00:00:03.000000000Z") != std::string_view::npos)
                ++overlap_queries;
            if (id.find("cancel-job") != std::string::npos) {
                if (log_attempts_[id]++ == 0)
                    return {200, "{\"entries\":[]}"};
                return {200, "{\"entries\":[{\"timestamp\":\"2026-01-01T00:00:05Z\","
                             "\"receiveTimestamp\":\"2026-01-01T00:00:05Z\","
                             "\"insertId\":\"cancel\",\"textPayload\":\"cancelled\"}]}"};
            }
            if (id.find("sink-failure") != std::string::npos) {
                if (log_attempts_[id]++ == 0)
                    return {200, "{\"entries\":[]}"};
                return {200, "{\"entries\":[{\"timestamp\":\"2026-01-01T00:00:06Z\","
                             "\"receiveTimestamp\":\"2026-01-01T00:00:06Z\","
                             "\"insertId\":\"sink\",\"textPayload\":\"late\"}]}"};
            }
            if (id.find("retry-success") == std::string::npos)
                return {200, "{\"entries\":[]}"};
            if (log_attempts_[id]++ == 0)
                return {500, "{\"error\":{\"message\":\"logging retry\"}}"};
            if (body.find("\"pageToken\":\"next\"") != std::string_view::npos)
                return {200, "{\"entries\":[{\"timestamp\":\"2026-01-01T00:00:02Z\","
                             "\"receiveTimestamp\":\"2026-01-01T00:00:02Z\","
                             "\"insertId\":\"b\",\"textPayload\":\"second\"},"
                             "{\"timestamp\":\"2026-01-01T00:00:03Z\","
                             "\"receiveTimestamp\":\"2026-01-01T00:00:03Z\","
                             "\"insertId\":\"a\",\"textPayload\":\"third\"},"
                             "{\"timestamp\":\"2026-01-01T00:00:04Z\","
                             "\"receiveTimestamp\":\"2026-01-01T00:00:04Z\","
                             "\"textPayload\":\"fourth\"}]}"};
            return {200, "{\"entries\":[{\"timestamp\":\"2026-01-01T00:00:01Z\","
                         "\"receiveTimestamp\":\"2026-01-01T00:00:01Z\","
                         "\"insertId\":\"a\",\"textPayload\":\"first\"}],"
                         "\"nextPageToken\":\"next\"}"};
        }

        if (method == "GET" && target.find("/tasks/0") != std::string_view::npos &&
            target.find("failed-exit-deadline") != std::string_view::npos)
            return {500, "{\"error\":{\"message\":\"task retry\"}}"};

        const std::string id = job_id(target);
        if (method == "POST" && ends_with(target, ":cancel")) {
            cancelled_[id] = true;
            if (id.find("ambiguous-cancel") != std::string::npos) {
                ++ambiguous_cancel_attempts;
                return {500, "{\"error\":{\"message\":\"lost cancel response\"}}"};
            }
            return {200, "{\"name\":\"projects/test-project/locations/europe-west4/"
                         "operations/cancel-" +
                             id + "\"}"};
        }
        if (method == "DELETE" && !id.empty()) {
            ++deletes;
            if (id.find("cleanup-failure") != std::string::npos)
                return {403, "{\"error\":{\"message\":\"denied\"}}"};
            return {200, "{\"name\":\"projects/test-project/locations/europe-west4/"
                         "operations/delete-" +
                             id + "\"}"};
        }
        if (method == "GET" && target.find("/operations/") != std::string_view::npos) {
            if (target.find("/cancel-") != std::string_view::npos)
                ++cancel_operations;
            return {200, "{\"done\":true,\"status\":\"DONE\",\"response\":{}}"};
        }
        if (method == "GET" && !id.empty()) {
            std::string state = "SUCCEEDED";
            if (id.find("failed-exit-deadline") != std::string::npos) {
                state = "FAILED";
            } else if (cancelled_[id] && id.find("ambiguous-cancel") != std::string::npos) {
                const int read = cancellation_reads_[id]++;
                state = read == 0   ? "RUNNING"
                        : read == 1 ? "CANCELLATION_IN_PROGRESS"
                                    : "CANCELLED";
            } else if (cancelled_[id]) {
                state = "CANCELLED";
            } else if (id.find("cancel") != std::string::npos ||
                       id.find("timeout") != std::string::npos) {
                state = "RUNNING";
            } else if (id.find("retry-success") != std::string::npos && job_reads_[id]++ == 0) {
                state = "RUNNING";
            }
            return {200, "{\"name\":\"projects/test-project/locations/europe-west4/jobs/" + id +
                             "\",\"uid\":\"uid-" + id + "\",\"status\":{\"state\":\"" + state +
                             "\"}}"};
        }
        return {404, "{\"error\":{\"message\":\"not found\"}}"};
    }

    static std::string read_request(int socket) {
        std::string request;
        std::array<char, 4096> buffer{};
        std::size_t wanted = 0;
        while (true) {
            const auto count = ::recv(socket, buffer.data(), buffer.size(), 0);
            if (count <= 0)
                break;
            request.append(buffer.data(), static_cast<std::size_t>(count));
            const auto header_end = request.find("\r\n\r\n");
            if (header_end == std::string::npos)
                continue;
            if (!wanted) {
                const auto field = request.find("Content-Length:");
                if (field != std::string::npos) {
                    const auto begin = request.find_first_not_of(' ', field + 15);
                    wanted = static_cast<std::size_t>(std::stoull(request.substr(begin)));
                }
            }
            if (request.size() >= header_end + 4 + wanted)
                break;
        }
        return request;
    }

    static void write_reply(int socket, const reply& response) {
        const std::string reason = response.status == 200 ? "OK" : "Error";
        std::string text = "HTTP/1.1 " + std::to_string(response.status) + ' ' + reason + "\r\n";
        bool has_content_type = false;
        bool has_content_length = false;
        for (const auto& field : response.headers) {
            std::string lower = field;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            has_content_type = has_content_type || starts_with(lower, "content-type:");
            has_content_length = has_content_length || starts_with(lower, "content-length:");
            text += field + "\r\n";
        }
        if (!has_content_type)
            text += "Content-Type: application/json\r\n";
        if (!has_content_length)
            text += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        text += "Connection: close\r\n\r\n" + response.body;
        std::size_t sent = 0;
        while (sent < text.size()) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const auto count = ::send(socket, text.data() + sent, text.size() - sent, flags);
            if (count <= 0)
                break;
            sent += static_cast<std::size_t>(count);
        }
    }

    void serve() {
        while (!stopping_) {
            const int socket = ::accept(listener_, nullptr, nullptr);
            if (socket < 0)
                break;
            const std::string request = read_request(socket);
            const auto line_end = request.find("\r\n");
            std::istringstream line(request.substr(0, line_end));
            std::string method;
            std::string target;
            line >> method >> target;
            const auto header_end = request.find("\r\n\r\n");
            const std::string_view headers =
                header_end == std::string::npos
                    ? std::string_view{}
                    : std::string_view(request).substr(line_end + 2, header_end - line_end - 2);
            const std::string_view body = header_end == std::string::npos
                                              ? std::string_view{}
                                              : std::string_view(request).substr(header_end + 4);
            const auto response = route(method, target, headers, body);
            if (response.status != 0)
                write_reply(socket, response);
            (void)::close(socket);
        }
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    std::map<std::string, int> create_attempts_;
    std::map<std::string, int> log_attempts_;
    std::map<std::string, int> job_reads_;
    std::map<std::string, int> cancellation_reads_;
    std::map<std::string, bool> cancelled_;
    std::map<std::string, std::string> request_ids_;
    std::map<std::string, std::string> aws_objects_;
    std::map<std::string, std::string> azure_objects_;
    std::string aws_compute_client_token_;
};

void transport_limit_tests() {
    fake_server server;

    const auto expect_body_limit = [](auto action, long status, std::string_view label) {
        bool caught = false;
        try {
            action();
        } catch (const cloud::error& failure) {
            caught = true;
            tst::check(std::string_view(failure.what()).find(
                           "HTTP response body exceeded the private byte limit") !=
                           std::string_view::npos &&
                           failure.http_status() == status && failure.response().size() <= 8 * 1024,
                       label);
        }
        tst::check(caught, label);
    };

    const auto exact = cloud::gcp::detail::http(
        cloud::gcp::detail::HttpRequest{}
            .with_url(server.url() + "/transport-body?bytes=32")
            .with_max_response_bytes(32));
    tst::check(exact.body == std::string(32, 'x'), "exact in-memory response limit");

    expect_body_limit(
        [&] {
            (void)cloud::gcp::detail::http(
                cloud::gcp::detail::HttpRequest{}
                    .with_url(server.url() + "/transport-body?bytes=33")
                    .with_max_response_bytes(32));
        },
        200, "oversized in-memory response rejected explicitly");

    const auto destination = std::filesystem::temp_directory_path() /
                             ("cloud-h-stream-limit-" + cloud::gcp::detail::random_uuid());
    cloud::gcp::detail::TemporaryPathGuard destination_guard(destination);
    cloud::gcp::detail::HttpRequest download;
    download.url = server.url() + "/transport-body?bytes=64";
    download.download_file = destination;
    download.max_response_bytes = 32;
    (void)cloud::gcp::detail::http(std::move(download));
    tst::check(cloud::gcp::detail::read_small_file(destination) == std::string(64, 'x'),
               "successful file response streams beyond memory limit");

    expect_body_limit(
        [&] {
            (void)cloud::gcp::detail::http(
                cloud::gcp::detail::HttpRequest{}.with_url(
                    server.url() + "/transport-body?bytes=65537&status=500"));
        },
        500, "oversized HTTP error response rejected explicitly");

    const auto error_destination = std::filesystem::temp_directory_path() /
                                   ("cloud-h-error-limit-" +
                                    cloud::gcp::detail::random_uuid());
    cloud::gcp::detail::TemporaryPathGuard error_destination_guard(error_destination);
    {
        std::ofstream existing(error_destination, std::ios::binary);
        existing << "sentinel";
    }
    const std::string part_prefix = error_destination.filename().string() + ".cloud-part-";
    expect_body_limit(
        [&] {
            cloud::gcp::detail::HttpRequest failed_download;
            failed_download.url = server.url() + "/transport-body?bytes=65537&status=500";
            failed_download.download_file = error_destination;
            (void)cloud::gcp::detail::http(std::move(failed_download));
        },
        500, "oversized streamed error response rejected explicitly");
    tst::check(cloud::gcp::detail::read_small_file(error_destination) == "sentinel",
               "streamed error preserves existing destination");
    for (const auto& entry : std::filesystem::directory_iterator(error_destination.parent_path()))
        tst::check(!starts_with(entry.path().filename().string(), part_prefix),
                   "streamed error removes temporary file");

    bool header_limit = false;
    try {
        (void)cloud::gcp::detail::http(
            cloud::gcp::detail::HttpRequest{}.with_url(server.url() + "/transport-headers"));
    } catch (const cloud::error& failure) {
        header_limit = true;
        tst::check(std::string_view(failure.what()).find(
                       "HTTP response headers exceeded the private byte limit") !=
                       std::string_view::npos &&
                       failure.http_status() == 200 && failure.response().empty(),
                   "oversized HTTP headers rejected explicitly");
    }
    tst::check(header_limit, "oversized HTTP headers rejected explicitly");

    const auto credential_path = std::filesystem::temp_directory_path() /
                                 ("cloud-h-credential-limit-" +
                                  cloud::gcp::detail::random_uuid());
    cloud::gcp::detail::TemporaryPathGuard credential_guard(credential_path);
    {
        std::ofstream credential(credential_path, std::ios::binary);
        credential << "{\"x\":1}";
    }
    (void)cloud::gcp::detail::read_json_file(credential_path, 7);
    tst::throws<cloud::error>(
        [&] { (void)cloud::gcp::detail::read_json_file(credential_path, 6); },
        "credential file byte limit");
}

void gcp_lifecycle_tests() {
    fake_server server;
    cloud::config config;
    config.project = "test-project";
    config.region = "europe-west4";
    config.zone = "europe-west4-a";
    config.auth = cloud::auth::bearer("test");
    config.allow_insecure_http = true;
    config.storage_endpoint = server.url();
    config.compute_endpoint = server.url();
    config.batch_endpoint = server.url();
    config.logging_endpoint = server.url();
    config.instance_templates["worker"].gcp_instance_template = "gcp-worker-template";
    config.request_timeout = std::chrono::milliseconds(100);
    config.poll_interval = std::chrono::milliseconds(1);
    config.final_log_delay = std::chrono::milliseconds(50);
    config.final_log_timeout = std::chrono::milliseconds(200);
    config.cleanup_timeout = std::chrono::seconds(2);
    cloud::client client(std::move(config));

    cloud::job_spec spec;
    spec.name = "retry-success";
    spec.image = "image";
    spec.command = {"run"};
    spec.service_account = "runner@test-project.iam.gserviceaccount.com";
    spec.timeout = std::chrono::seconds(5);
    auto success = client.run(spec);
    std::vector<std::string> output;
    const auto result = success.wait([&](const auto& line) { output.push_back(line.text); });
    tst::check(result.success(), "HTTP success result");
    tst::check(output == std::vector<std::string>({"first", "second", "third", "fourth"}),
               "composite log identity and fallback deduplication");
    tst::check(success.logs().size() == 4, "cached logs after deletion");
    tst::check(server.create_retries == 1 && !server.request_id_changed, "idempotent create retry");
    tst::check(server.cursor_queries > 0, "incremental log cursor");
    tst::check(server.overlap_queries > 0, "log cursor retains an overlap entry");

    spec.name = "cancel-job";
    auto cancelled = client.run(spec);
    cancelled.cancel();
    tst::check(cancelled.wait().state == cloud::job_state::cancelled, "observed cancellation");
    tst::check(cancelled.logs().size() == 1 && cancelled.logs().front().text == "cancelled",
               "cancellation drains delayed final logs");
    tst::check(server.cancel_operations > 0, "cancel operation polled");

    spec.name = "ambiguous-cancel";
    auto ambiguous = client.run(spec);
    ambiguous.cancel();
    tst::check(ambiguous.wait().state == cloud::job_state::cancelled &&
                   server.ambiguous_cancel_attempts == 4,
               "accepted cancellation survives a lost response");

    spec.name = "timeout-job";
    spec.timeout = std::chrono::milliseconds(1);
    auto timed_out = client.run(spec);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto timeout = timed_out.wait();
    tst::check(timeout.state == cloud::job_state::cancelled &&
                   timeout.error().find("timeout") != std::string::npos,
               "controller timeout cancellation");

    spec.name = "cleanup-failure";
    spec.timeout = std::chrono::seconds(5);
    auto cleanup = client.run(spec);
    const auto preserved = cleanup.wait();
    tst::check(preserved.success() && !preserved.warnings.empty(),
               "cleanup failure preserves result");
    tst::check(server.deletes >= 5, "automatic deletion attempted");

    spec.name = "failed-exit-deadline";
    spec.auto_delete = false;
    const auto started = std::chrono::steady_clock::now();
    const auto failed = client.run(spec).wait();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    tst::check(failed.state == cloud::job_state::failed && !failed.exit_code &&
                   elapsed < std::chrono::milliseconds(500),
               "failed-task exit lookup respects its deadline");

    spec.name = "sink-failure";
    spec.auto_delete = true;
    bool sink_failure = false;
    try {
        (void)client.run(spec).wait([](const auto&) { throw std::runtime_error("sink failure"); });
    } catch (const std::runtime_error& failure) {
        sink_failure = std::string_view(failure.what()) == "sink failure";
    }
    tst::check(sink_failure, "final-drain callback exceptions propagate");

    const auto stored = client.storage().put("cloud://test-bucket/object", "payload");
    tst::check(stored.name == "object" && stored.size == 7, "storage upload metadata");
    tst::check(client.storage().get("cloud://test-bucket/object") == "payload",
               "verified storage download");
    tst::check(client.storage().stat("cloud://test-bucket/object").generation == "7",
               "storage stat forwarding");

    const int lists_before = server.storage_lists;
    const auto objects = client.storage().list("cloud://test-bucket");
    tst::check(objects.objects.size() == 2 && objects.objects[0].name == "first" &&
                   objects.objects[1].name == "second" &&
                   objects.prefixes == std::vector<std::string>{"dir/"} &&
                   server.storage_lists == lists_before + 2,
               "storage pagination");
    cloud::list_options limited;
    limited.limit = 1;
    const int limited_before = server.storage_lists;
    tst::check(client.storage().list("cloud://test-bucket", limited).objects.size() == 1 &&
                   server.storage_lists == limited_before + 1,
               "storage pagination limit");

    const auto destination = std::filesystem::temp_directory_path() /
                             ("cloud-h-test-" + cloud::gcp::detail::random_uuid());
    cloud::gcp::detail::TemporaryPathGuard file_guard(destination);
    client.storage().get_file("cloud://test-bucket/object", destination);
    tst::check(cloud::gcp::detail::read_small_file(destination) == "payload",
               "verified file download");
    {
        std::ofstream file_output(destination, std::ios::binary | std::ios::trunc);
        file_output << "preserved";
    }
    server.corrupt_download = true;
    tst::throws<cloud::error>(
        [&] { client.storage().get_file("cloud://test-bucket/object", destination); },
        "file download rejects a corrupt checksum");
    server.corrupt_download = false;
    tst::check(cloud::gcp::detail::read_small_file(destination) == "preserved",
               "corrupt file download preserves destination");
    client.storage().remove("cloud://test-bucket/object");
    tst::throws<cloud::error>([&] { (void)client.storage().get("cloud://test-bucket"); },
                              "storage object path validation");

    const auto instances = client.compute().instances();
    tst::check(instances.size() == 2 && instances[0].name == "vm-a" && instances[1].name == "vm-b",
               "compute pagination");

    const auto created = client.compute().create("gcp-created", "worker");
    tst::check(created.name() == "insert-gcp-created" && created.zone() == "europe-west4-a",
               "GCP logical template creates a portable operation");
    created.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));
    tst::check(server.gcp_compute_creates == 1 && server.gcp_compute_valid,
               "GCP raw compute resolves the configured native template");
    tst::throws<cloud::error>([&] { (void)client.compute().create("vm", "missing"); },
                              "GCP raw compute rejects an unknown logical template");
}

cloud::config aws_storage_config(const fake_server& server) {
    cloud::config config;
    config.provider = "aws";
    config.region = "eu-west-1";
    config.allow_insecure_http = true;
    config.aws.access_key_id = "AKIDEXAMPLE";
    config.aws.secret_access_key = "secret";
    config.aws.s3_endpoint = server.url() + "/s3";
    config.request_timeout = std::chrono::seconds(2);
    config.transfer_timeout = std::chrono::seconds(2);
    return config;
}

cloud::config azure_storage_config(const fake_server& server) {
    cloud::config config;
    config.provider = "azure";
    config.region = "westeurope";
    config.allow_insecure_http = true;
    config.azure.storage_endpoint = server.url() + "/blob";
    config.azure.storage_auth = cloud::auth::bearer("azure-storage-token");
    config.request_timeout = std::chrono::seconds(2);
    config.transfer_timeout = std::chrono::seconds(2);
    return config;
}

void aws_storage_tests() {
    fake_server server;
    cloud::client client(aws_storage_config(server));

    cloud::put_options create_only;
    create_only.content_type = "text/plain";
    create_only.if_generation_match = "0";
    const auto stored = client.storage().put("cloud://test-bucket/object", "payload", create_only);
    tst::check(stored.name == "object" && stored.generation == "\"aws-version\"" &&
                   stored.size == 7 && stored.crc32c == cloud::gcp::detail::crc32c("payload"),
               "AWS S3 put returns portable metadata");
    const auto empty = client.storage().put("cloud://test-bucket/empty", "", create_only);
    tst::check(empty.size == 0 && empty.crc32c == cloud::gcp::detail::crc32c(""),
               "AWS S3 put transmits an explicit empty object");
    tst::check(client.storage().get("cloud://test-bucket/object") == "payload",
               "AWS S3 get verifies the stored checksum");
    const auto metadata = client.storage().stat("cloud://test-bucket/object");
    tst::check(metadata.etag == "\"aws-version\"" && metadata.size == 7,
               "AWS S3 stat maps ETag and size");

    cloud::list_options listing;
    listing.delimiter = "/";
    const auto objects = client.storage().list("cloud://test-bucket/dir/", listing);
    tst::check(objects.objects.size() == 2 && objects.objects[0].name == "dir/first file" &&
                   objects.objects[1].name == "dir/second" &&
                   objects.prefixes == std::vector<std::string>{"dir/sub/"} &&
                   server.aws_storage_lists == 2,
               "AWS S3 list decodes keys and follows continuation tokens");

    const auto source = std::filesystem::temp_directory_path() /
                        ("cloud-h-aws-source-" + cloud::gcp::detail::random_uuid());
    const auto destination = std::filesystem::temp_directory_path() /
                             ("cloud-h-aws-destination-" + cloud::gcp::detail::random_uuid());
    cloud::gcp::detail::TemporaryPathGuard source_guard(source);
    cloud::gcp::detail::TemporaryPathGuard destination_guard(destination);
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "file-payload";
    }
    cloud::put_options replace;
    replace.if_generation_match = "\"aws-version\"";
    const auto file = client.storage().put_file("cloud://test-bucket/file", source, replace);
    tst::check(file.size == 12 && server.aws_storage_file_puts == 1,
               "AWS streamed file upload uses PUT");
    client.storage().get_file("cloud://test-bucket/file", destination);
    tst::check(cloud::gcp::detail::read_small_file(destination) == "file-payload",
               "AWS S3 get_file streams a verified file");
    {
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << "preserved";
    }
    server.corrupt_download = true;
    tst::throws<cloud::error>(
        [&] { client.storage().get_file("cloud://test-bucket/file", destination); },
        "AWS S3 get_file rejects a corrupt checksum");
    server.corrupt_download = false;
    tst::check(cloud::gcp::detail::read_small_file(destination) == "preserved",
               "AWS corrupt download preserves its destination");

    client.storage().remove("cloud://test-bucket/object");
    client.storage().remove("cloud://test-bucket/empty");
    tst::throws<cloud::error>([&] { (void)client.storage().stat("cloud://test-bucket/object"); },
                              "AWS S3 remove deletes the routed object");
    tst::check(server.aws_storage_puts == 2 && server.aws_storage_deletes == 2 &&
                   server.aws_storage_gets >= 3,
               "AWS S3 facade issued the expected operations");
    tst::check(server.aws_storage_valid,
               "AWS S3 requests carry signing, conditions, and checksum metadata");
}

void azure_storage_tests() {
    fake_server server;
    cloud::client client(azure_storage_config(server));

    cloud::put_options create_only;
    create_only.content_type = "text/plain";
    create_only.if_generation_match = "0";
    const auto stored =
        client.storage().put("cloud://test-container/object", "payload", create_only);
    tst::check(stored.name == "object" && stored.generation == "\"azure-version\"" &&
                   stored.size == 7 && stored.crc32c == cloud::gcp::detail::crc32c("payload"),
               "Azure Blob put returns portable metadata");
    const auto empty = client.storage().put("cloud://test-container/empty", "", create_only);
    tst::check(empty.size == 0 && empty.crc32c == cloud::gcp::detail::crc32c(""),
               "Azure Blob put transmits an explicit empty object");
    tst::check(client.storage().get("cloud://test-container/object") == "payload",
               "Azure Blob get verifies the stored checksum");
    const auto metadata = client.storage().stat("cloud://test-container/object");
    tst::check(metadata.etag == "\"azure-version\"" && metadata.size == 7,
               "Azure Blob stat maps ETag and size");

    cloud::list_options listing;
    listing.delimiter = "/";
    const auto objects = client.storage().list("cloud://test-container/dir/", listing);
    tst::check(objects.objects.size() == 2 &&
                   objects.objects[0].name == "dir/first & data" &&
                   objects.objects[0].crc32c == "AAAAAA==" &&
                   objects.objects[1].name == "dir/second" &&
                   objects.prefixes == std::vector<std::string>{"dir/sub/"} &&
                   server.azure_storage_lists == 2,
               "Azure Blob list decodes names, parses metadata, and follows markers");

    const auto source = std::filesystem::temp_directory_path() /
                        ("cloud-h-azure-source-" + cloud::gcp::detail::random_uuid());
    const auto destination = std::filesystem::temp_directory_path() /
                             ("cloud-h-azure-destination-" + cloud::gcp::detail::random_uuid());
    cloud::gcp::detail::TemporaryPathGuard source_guard(source);
    cloud::gcp::detail::TemporaryPathGuard destination_guard(destination);
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "file-payload";
    }
    cloud::put_options replace;
    replace.if_generation_match = "\"azure-version\"";
    const auto file = client.storage().put_file("cloud://test-container/file", source, replace);
    tst::check(file.size == 12 && server.azure_storage_file_puts == 1,
               "Azure streamed file upload uses PUT");
    client.storage().get_file("cloud://test-container/file", destination);
    tst::check(cloud::gcp::detail::read_small_file(destination) == "file-payload",
               "Azure Blob get_file streams a verified file");
    {
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << "preserved";
    }
    server.corrupt_download = true;
    tst::throws<cloud::error>(
        [&] { client.storage().get_file("cloud://test-container/file", destination); },
        "Azure Blob get_file rejects a corrupt checksum");
    server.corrupt_download = false;
    tst::check(cloud::gcp::detail::read_small_file(destination) == "preserved",
               "Azure corrupt download preserves its destination");

    client.storage().remove("cloud://test-container/object");
    client.storage().remove("cloud://test-container/empty");
    tst::throws<cloud::error>(
        [&] { (void)client.storage().stat("cloud://test-container/object"); },
        "Azure Blob remove deletes the routed object");
    tst::check(server.azure_storage_puts == 2 && server.azure_storage_deletes == 2 &&
                   server.azure_storage_gets >= 3,
               "Azure Blob facade issued the expected operations");
    tst::check(server.azure_storage_valid,
               "Azure Blob requests carry bearer, conditions, and checksum metadata");
}

void storage_route_tests() {
    fake_server server;
    auto config = aws_storage_config(server);
    config.provider.reset();
    config.providers = {"aws", "azure"};
    config.azure.storage_endpoint = server.url() + "/blob";
    config.azure.storage_auth = cloud::auth::bearer("azure-storage-token");
    cloud::client router(std::move(config));
    tst::throws<cloud::error>([&] { (void)router.storage().stat("cloud://test-bucket/object"); },
                              "multi-provider storage requires a route");

    cloud::put_options create_only;
    create_only.if_generation_match = "0";
    const auto aws = router.route("aws");
    const auto azure = router.route("azure");
    tst::check(aws.storage().put("cloud://test-bucket/object", "payload", create_only).name ==
                   "object" &&
                   azure.storage()
                           .put("cloud://test-container/object", "payload", create_only)
                           .name == "object" &&
                   server.aws_storage_puts == 1 && server.azure_storage_puts == 1,
               "route(provider) binds cloud URIs to exactly one storage backend");
}

cloud::config aws_compute_config(const fake_server& server) {
    cloud::config config;
    config.provider = "aws";
    config.region = "eu-west-1";
    // Deliberately differ from the returned instance AZ: operation locations
    // must report provider state rather than echoing this fallback.
    config.zone = "eu-west-1b";
    config.allow_insecure_http = true;
    config.aws.access_key_id = "AKIDEXAMPLE";
    config.aws.secret_access_key = "secret";
    config.aws.ec2_endpoint = server.url() + "/ec2";
    config.instance_templates["worker"].aws.id = "lt-worker";
    config.instance_templates["worker"].aws.version = "7";
    config.request_timeout = std::chrono::seconds(2);
    return config;
}

void aws_compute_tests() {
    fake_server server;
    cloud::client client(aws_compute_config(server));

    const auto instances = client.compute().instances();
    tst::check(instances.size() == 2 && instances[0].id == "i-list-a" &&
                   instances[0].name == "list-a" && instances[0].status == "stopped" &&
                   instances[0].zone == "eu-west-1a" &&
                   instances[0].machine_type == "m7i.large" &&
                   instances[1].name == "list-b" && instances[1].status == "running" &&
                   server.aws_compute_lists == 2,
               "AWS EC2 list maps tagged instances and follows NextToken");

    const auto created = client.compute().create("aws-created", "worker");
    tst::check(created.name() == "i-created" && created.zone() == "eu-west-1a",
               "AWS create returns a provider-neutral operation");
    created.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));
    tst::check(server.aws_compute_create_attempts == 2 &&
                   server.aws_compute_create_polls >= 2,
               "AWS RunInstances reuses its idempotency token and polls pending to running");
    const auto clock_origin = std::chrono::steady_clock::time_point{};
    tst::check(cloud::detail::bounded_retry_delay(
                   0, clock_origin + std::chrono::milliseconds(20), clock_origin) ==
                   std::chrono::milliseconds(20) &&
                   cloud::detail::bounded_retry_delay(
                       0, std::chrono::steady_clock::time_point::max(), clock_origin) ==
                       std::chrono::milliseconds(100),
               "provider retry delays are deterministically capped by operation deadlines");
    server.aws_compute_retry_wait = true;
    tst::throws<cloud::error>(
        [&] { created.wait(std::chrono::milliseconds(20), std::chrono::milliseconds(1)); },
        "AWS operation retry honours its wait deadline");
    server.aws_compute_retry_wait = false;

    const auto started = client.compute().start("aws-start");
    tst::check(started.name() == "i-start" && started.zone() == "eu-west-1a",
               "AWS start resolves a managed logical name and its actual zone");
    started.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));

    const auto prefixed = client.compute().start("i-worker");
    tst::check(prefixed.name() == "i-0123456789abcdef0" &&
                   prefixed.zone() == "eu-west-1a",
               "AWS logical names beginning i- remain managed Name-tag lookups");
    prefixed.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));

    const auto stopped = client.compute().stop("aws-stop");
    tst::check(stopped.name() == "i-stop" && stopped.zone() == "eu-west-1a",
               "AWS stop resolves a managed logical name and its actual zone");
    stopped.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));

    const auto destroyed = client.compute().destroy("aws-destroy");
    tst::check(destroyed.name() == "i-destroy" && destroyed.zone() == "eu-west-1a",
               "AWS destroy resolves a managed logical name and its actual zone");
    destroyed.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));

    tst::check(server.aws_compute_starts == 2 && server.aws_compute_start_polls >= 3 &&
                   server.aws_compute_stops == 1 && server.aws_compute_stop_polls >= 2 &&
                   server.aws_compute_destroys == 1 &&
                   server.aws_compute_destroy_polls >= 2 && server.aws_compute_valid,
               "AWS raw-instance actions sign requests and observe their terminal states");
    tst::throws<cloud::error>(
        [&] { (void)client.compute().create("aws-missing", "missing"); },
        "AWS create rejects an unknown logical template before HTTP");
    tst::throws<cloud::error>(
        [&] { (void)client.compute().create("aws-existing", "worker"); },
        "AWS create rejects an existing managed Name tag");
    tst::throws<cloud::error>(
        [&] { (void)client.compute().create("i-0123456789abcdef0", "worker"); },
        "AWS create reserves exact EC2 instance-ID syntax");
    tst::throws<cloud::error>([&] { (void)client.compute().start("aws-ambiguous"); },
                              "AWS name resolution detects ambiguity across pages");
}

cloud::config azure_compute_config(const fake_server& server) {
    cloud::config config;
    config.provider = "azure";
    config.region = "westeurope";
    config.allow_insecure_http = true;
    config.auth = cloud::auth::bearer("azure-batch-token");
    config.azure.storage_auth = cloud::auth::bearer("azure-storage-token");
    config.azure.management_auth = cloud::auth::bearer("azure-management-token");
    config.azure.management_endpoint = server.url() + "/arm";
    config.azure.subscription_id = "subscription";
    config.azure.resource_group = "workers";
    config.azure.compute_api_version = "2025-04-01";
    auto& native = config.instance_templates["worker"].azure;
    native.image_id = "/images/worker";
    native.subnet_id = "/networks/subnets/workers";
    native.machine_type = "Standard_D4s_v5";
    native.location = "uksouth";
    native.os_disk_type = "Premium_LRS";
    config.request_timeout = std::chrono::seconds(2);
    return config;
}

void azure_compute_tests() {
    fake_server server;
    cloud::client client(azure_compute_config(server));

    server.azure_cross_origin_next = true;
    tst::throws<cloud::error>([&] { (void)client.compute().instances(); },
                              "Azure list rejects a cross-origin nextLink");
    server.azure_cross_origin_next = false;

    const auto instances = client.compute().instances();
    tst::check(instances.size() == 2 && instances[0].name == "azure-list-a" &&
                   instances[0].machine_type == "Standard_D4s_v5" &&
                   instances[0].status == "stopped" &&
                   instances[1].name == "azure-list-b" && instances[1].status == "running" &&
                   server.azure_compute_lists == 3,
               "Azure VM list maps power states and follows a same-origin nextLink");

    const auto created = client.compute().create("azure-created", "worker");
    tst::check(created.name() == "azure-created" && created.location() == "uksouth",
               "Azure create returns a portable operation with its native location");
    created.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));
    server.azure_compute_retry_wait = true;
    tst::throws<cloud::error>(
        [&] { created.wait(std::chrono::milliseconds(20), std::chrono::milliseconds(1)); },
        "Azure operation retry honours its wait deadline");
    server.azure_compute_retry_wait = false;

    const auto started = client.compute().start("azure-start");
    started.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));
    const auto stopped = client.compute().stop("azure-stop");
    stopped.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));
    const auto destroyed = client.compute().destroy("azure-destroy");
    destroyed.wait(std::chrono::seconds(1), std::chrono::milliseconds(1));

    server.azure_cross_origin_operation = true;
    const auto untrusted = client.compute().create("azure-cross", "worker");
    server.azure_cross_origin_operation = false;
    tst::throws<cloud::error>(
        [&] { untrusted.wait(std::chrono::seconds(1), std::chrono::milliseconds(1)); },
        "Azure operation polling rejects a cross-origin asynchronous URL");

    tst::check(server.azure_compute_creates == 2 && server.azure_compute_starts == 1 &&
                   server.azure_compute_deallocates == 1 && server.azure_compute_deletes == 1 &&
                   server.azure_compute_operation_polls >= 5 && server.azure_compute_valid,
               "Azure raw compute uses inline image/network definitions, deallocation, and a "
               "management-scoped bearer");
    tst::throws<cloud::error>(
        [&] { (void)client.compute().create("azure-missing", "missing"); },
        "Azure create rejects an unknown logical template before HTTP");
}

cloud::config aws_mount_config(const fake_server& server) {
    cloud::config config;
    config.provider = "aws";
    config.region = "eu-west-1";
    config.allow_insecure_http = true;
    config.aws.access_key_id = "AKIDEXAMPLE";
    config.aws.secret_access_key = "secret";
    config.aws.batch_endpoint = server.url();
    config.aws.logs_endpoint = server.url();
    config.aws.fargate_job_queue = "fargate-queue";
    config.aws.execution_role_arn = "arn:aws:iam::1:role/execution";
    config.aws.job_role_arn = "arn:aws:iam::1:role/job";
    config.aws.s3_files["input-bucket"] = {
        "arn:aws:s3files:eu-west-1:1:file-system/fs-test", {}};
    config.poll_interval = std::chrono::milliseconds(1);
    config.final_log_delay = std::chrono::milliseconds(1);
    config.final_log_timeout = std::chrono::milliseconds(20);
    return config;
}

cloud::job_spec aws_mount_spec() {
    cloud::job_spec spec;
    spec.name = "aws-mounted";
    spec.image = "image";
    spec.command = {"run"};
    spec.resources.cpus = 2;
    spec.resources.memory_gb = 4;
    spec.timeout = std::chrono::seconds(60);
    spec.mounts.push_back({"cloud://input-bucket/prefix/", "/inputs", true});
    return spec;
}

void aws_mount_tests() {
    fake_server server;
    cloud::client client(aws_mount_config(server));
    const auto spec = aws_mount_spec();

    auto gpu = spec;
    gpu.resources.gpu = "nvidia-l4";
    tst::throws<cloud::error>([&] { (void)client.run(gpu); },
                              "AWS mounted GPU jobs fail closed");
    tst::check(server.aws_registers == 0, "AWS mounted GPU rejection precedes HTTP");

    auto missing_queue_config = aws_mount_config(server);
    missing_queue_config.aws.fargate_job_queue.clear();
    cloud::client missing_queue(std::move(missing_queue_config));
    tst::throws<cloud::error>([&] { (void)missing_queue.run(spec); },
                              "AWS mounted jobs require an explicit Fargate queue");
    tst::check(server.aws_registers == 0, "AWS missing-queue rejection precedes HTTP");

    const auto chosen = client.plan(spec);
    tst::check(chosen.machine_type == "FARGATE" && chosen.accelerator.empty(),
               "AWS mounted planning selects Fargate without accelerators");

    auto priced_config = aws_mount_config(server);
    std::optional<cloud::price_request> priced_request;
    priced_config.lookup_hourly_cost = [&](const cloud::price_request& request) {
        priced_request = request;
        return std::optional<double>(0.25);
    };
    cloud::client priced(std::move(priced_config));
    auto rounded = spec;
    rounded.resources.memory_gb = 4.1;
    const auto priced_plan = priced.plan(rounded);
    tst::check(priced_plan.estimated_hourly_cost == 0.25 && priced_request &&
                   priced_request->machine_type == "FARGATE" && priced_request->cpus == 2 &&
                   priced_request->memory_gb == 5,
               "AWS Fargate callback receives billed vCPU and rounded memory quantities");

    auto catalogue_config = aws_mount_config(server);
    catalogue_config.aws.pricing_endpoint = server.url();
    catalogue_config.prices = cloud::price_source::public_catalogue;
    cloud::client catalogue(std::move(catalogue_config));
    const auto rounded_catalogue = catalogue.plan(rounded);
    const auto exact_catalogue = catalogue.plan(spec);
    (void)catalogue.plan(rounded);
    tst::check(rounded_catalogue.estimated_hourly_cost &&
                   std::fabs(*rounded_catalogue.estimated_hourly_cost - 0.105) < 1e-12 &&
                   exact_catalogue.estimated_hourly_cost &&
                   std::fabs(*exact_catalogue.estimated_hourly_cost - 0.100) < 1e-12,
               "AWS Fargate catalogue composes vCPU and rounded-memory prices");
    tst::check(server.aws_fargate_price_reads == 4 && server.aws_fargate_price_valid,
               "AWS Fargate catalogue paginates and caches by billed shape");

    auto spot_config = aws_mount_config(server);
    spot_config.aws.fargate_spot_job_queue = "fargate-spot-queue";
    spot_config.aws.pricing_endpoint = server.url();
    spot_config.prices = cloud::price_source::public_catalogue;
    cloud::client spot_catalogue(std::move(spot_config));
    auto spot = spec;
    spot.resources.spot = true;
    tst::check(!spot_catalogue.plan(spot).estimated_hourly_cost &&
                   server.aws_fargate_price_reads == 4 && server.aws_spot_reads == 0,
               "AWS Fargate Spot remains unavailable without an unstable web-price lookup");
    spot.resources.max_price_per_hour = 1;
    tst::throws<cloud::error>([&] { (void)spot_catalogue.plan(spot); },
                              "AWS Fargate Spot price ceiling fails closed");

    const auto job = client.run(spec);
    tst::check(job.wait().success() && server.aws_fargate_registers == 1 &&
                   server.aws_fargate_submits == 1 && server.aws_mount_body_valid,
               "AWS S3 Files submission carries Fargate volumes, roles, mounts, and queue");
}

cloud::config azure_mount_config(const fake_server& server) {
    cloud::config config;
    config.provider = "azure";
    config.region = "westeurope";
    config.allow_insecure_http = true;
    config.auth = cloud::auth::bearer("azure-token");
    config.azure.batch_endpoint = server.url();
    config.azure.storage_account = "storageaccount";
    config.azure.storage_sas = "sv=test&sig=secret";
    config.poll_interval = std::chrono::milliseconds(1);
    config.final_log_delay = std::chrono::milliseconds(1);
    config.final_log_timeout = std::chrono::milliseconds(20);
    return config;
}

cloud::job_spec azure_mount_spec() {
    cloud::job_spec spec;
    spec.name = "azure-mounted";
    spec.image = "image";
    spec.command = {"run", "--fast"};
    spec.resources.cpus = 4;
    spec.resources.memory_gb = 16;
    spec.timeout = std::chrono::seconds(60);
    spec.mounts.push_back({"cloud://input-container", "/inputs", true});
    return spec;
}

void azure_mount_tests() {
    fake_server server;
    cloud::client client(azure_mount_config(server));
    const auto spec = azure_mount_spec();

    auto prefix = spec;
    prefix.mounts.front().source = "cloud://input-container/prefix/";
    tst::throws<cloud::error>([&] { (void)client.run(prefix); },
                              "Azure Blob mounts reject prefix-only sources");
    tst::check(server.azure_jobs == 0, "Azure prefix rejection precedes HTTP");

    const auto job = client.run(spec);
    tst::check(job.wait().success() && server.azure_mount_jobs == 1 &&
                   server.azure_mount_tasks == 1 && server.azure_mount_body_valid,
               "Azure submission carries BlobFuse configuration, volume, and admin identity");
}

void aws_lifecycle_tests() {
    fake_server server;

    cloud::config aws_config;
    aws_config.provider = "aws";
    aws_config.region = "eu-west-1";
    aws_config.allow_insecure_http = true;
    aws_config.aws.access_key_id = "AKIDEXAMPLE";
    aws_config.aws.secret_access_key = "secret";
    aws_config.aws.batch_endpoint = server.url();
    aws_config.aws.logs_endpoint = server.url();
    cloud::aws_gpu_target gpu_target;
    gpu_target.job_queue = "gpu-queue";
    gpu_target.machine_type = "g6.xlarge";
    gpu_target.cpus = 4;
    gpu_target.memory_gb = 16;
    gpu_target.gpus = 1;
    aws_config.aws.gpu_targets["l4"] = std::move(gpu_target);
    aws_config.poll_interval = std::chrono::milliseconds(1);
    aws_config.final_log_delay = std::chrono::milliseconds(1);
    aws_config.final_log_timeout = std::chrono::milliseconds(20);
    cloud::client aws(std::move(aws_config));
    cloud::job_spec aws_spec;
    aws_spec.name = "aws-gpu";
    aws_spec.image = "image";
    aws_spec.command = {"run"};
    aws_spec.resources.cpus = 4;
    aws_spec.resources.memory_gb = 16;
    aws_spec.resources.gpu = "nvidia-l4";
    aws_spec.retries = 1;
    aws_spec.timeout = std::chrono::seconds(60);
    const auto aws_plan = aws.plan(aws_spec);
    tst::check(aws_plan.provider == "aws" && aws_plan.machine_type == "g6.xlarge" &&
                   aws_plan.accelerator == "l4",
               "AWS GPU queue planning");
    auto aws_overcommit = aws_spec;
    aws_overcommit.resources.gpu_count = 2;
    tst::throws<cloud::error>([&] { (void)aws.plan(aws_overcommit); }, "AWS GPU target capacity");
    const auto aws_job = aws.run(aws_spec);
    const auto aws_result = aws_job.wait();
    tst::check(aws_result.success() && aws_job.logs().size() == 1 &&
                   aws_job.logs().front().text == "aws-out",
               "AWS Batch lifecycle and CloudWatch logs");
    tst::check(server.aws_registers == 1 && server.aws_submits == 1 &&
                   server.aws_deregisters == 1 && server.aws_log_reads >= 2 &&
                   server.aws_body_valid,
               "AWS REST request contract");

    const int recovered_registers = server.aws_registers;
    const int recovered_definitions = server.aws_definition_reads;
    const int recovered_submits = server.aws_submits;
    const int recovered_deregisters = server.aws_deregisters;
    server.aws_ambiguous_register = true;
    const auto recovered_job = aws.run(aws_spec);
    server.aws_ambiguous_register = false;
    tst::check(recovered_job.wait().success() && server.aws_registers == recovered_registers + 1 &&
                   server.aws_definition_reads == recovered_definitions + 1 &&
                   server.aws_submits == recovered_submits + 1 &&
                   server.aws_deregisters == recovered_deregisters + 1 && server.aws_body_valid,
               "AWS registration recovery by unique definition name");

    const auto paged_job = aws.run(aws_spec);
    const int capped_log_reads = server.aws_log_reads;
    server.aws_endless_logs = true;
    tst::throws<cloud::error>([&] { (void)paged_job.logs(); }, "AWS CloudWatch pagination cap");
    server.aws_endless_logs = false;
    tst::check(server.aws_log_reads == capped_log_reads + 100 && paged_job.logs().size() == 1 &&
                   paged_job.wait().success(),
               "AWS CloudWatch pagination failure preserves cursor state");
    const auto cancellation_retry = [&](int mode, std::string_view label) {
        auto retry_job = aws.run(aws_spec);
        server.aws_cancel_requests = 0;
        server.aws_cancel_accepted = false;
        server.aws_cancel_mode = mode;
        retry_job.cancel();
        tst::check(retry_job.status() == cloud::job_state::cancelled &&
                       server.aws_cancel_requests == 2 && server.aws_cancel_accepted,
                   label);
        server.aws_cancel_mode = 0;
    };
    cancellation_retry(1, "AWS CancelJob retries an ambiguous transport failure");
    cancellation_retry(2, "AWS TerminateJob retries an ambiguous transport failure");
    tst::check(aws.supports("aws", cloud::feature::containers) &&
                   aws.supports("aws", cloud::feature::accelerators) &&
                   aws.supports("aws", cloud::feature::object_storage) &&
                   aws.supports("aws", cloud::feature::storage_mounts),
               "AWS capabilities");
}

void azure_lifecycle_tests() {
    fake_server server;

    cloud::config azure_config;
    azure_config.provider = "azure";
    azure_config.region = "europe";
    azure_config.auth = cloud::auth::bearer("azure-token");
    azure_config.allow_insecure_http = true;
    azure_config.azure.batch_endpoint = server.url();
    azure_config.poll_interval = std::chrono::milliseconds(1);
    azure_config.final_log_delay = std::chrono::milliseconds(1);
    azure_config.final_log_timeout = std::chrono::milliseconds(20);
    cloud::client azure(std::move(azure_config));
    cloud::job_spec azure_spec;
    azure_spec.name = "azure-gpu";
    azure_spec.image = "image";
    azure_spec.command = {"run", "--fast"};
    azure_spec.resources.cpus = 16;
    azure_spec.resources.memory_gb = 64;
    azure_spec.resources.gpu = "a100";
    azure_spec.resources.spot = true;
    azure_spec.retries = 10;
    azure_spec.timeout = std::chrono::seconds(5);
    const auto azure_plan = azure.plan(azure_spec);
    tst::check(azure_plan.region == "westeurope" &&
                   azure_plan.machine_type == "Standard_NC24ads_A100_v4",
               "Azure GPU mapping");
    const auto azure_job = azure.run(azure_spec);
    const auto azure_result = azure_job.wait();
    const auto azure_logs = azure_job.logs();
    tst::check(azure_result.success() && azure_logs.size() == 3 &&
                   azure_logs[0].text == "attempt-two" && azure_logs[1].text == "attempt-restart" &&
                   azure_logs[2].text == "attempt-ten",
               "Azure Batch lifecycle and restart-safe task-file logs");
    tst::check(server.azure_jobs == 1 && server.azure_tasks == 1 && server.azure_terminates == 1 &&
                   server.azure_deletes == 1 && server.azure_delete_polls >= 2 &&
                   server.azure_body_valid,
               "Azure REST request contract");
}

void pricing_tests() {
    fake_server server;

    cloud::job_spec price_spec;
    price_spec.image = "image";
    price_spec.command = {"run"};
    price_spec.resources.cpus = 4;
    price_spec.resources.memory_gb = 16;

    cloud::config gcp_prices;
    gcp_prices.provider = "gcp";
    gcp_prices.project = "test";
    gcp_prices.region = "europe";
    gcp_prices.auth = cloud::auth::bearer("gcp-token");
    gcp_prices.allow_insecure_http = true;
    gcp_prices.billing_endpoint = server.url();
    gcp_prices.prices = cloud::price_source::public_catalogue;
    cloud::client gcp_priced(std::move(gcp_prices));
    tst::check(std::fabs(*gcp_priced.plan(price_spec).estimated_hourly_cost - 0.072) < 1e-12,
               "GCP built-in component price");
    auto fractional_timeout = price_spec;
    fractional_timeout.timeout = std::chrono::milliseconds(1'501);
    const auto fractional_diagnostics =
        gcp_priced.diagnose(fractional_timeout, std::chrono::seconds(1));
    tst::check(fractional_diagnostics.provider_attempt_timeout == std::chrono::seconds(2) &&
                   cloud::detail::batch_body(fractional_timeout,
                                             fractional_diagnostics.selected_plan)
                           .find("\"maxRunDuration\":\"2s\"") != std::string::npos,
               "diagnostic and GCP payload share ceil-second timeout rounding");
    const auto boundary_diagnostics =
        gcp_priced.diagnose(fractional_timeout, fractional_timeout.timeout);
    tst::check(boundary_diagnostics.expected_attempt_runtime == fractional_timeout.timeout,
               "expected attempt runtime may equal the controller timeout");
    tst::check(server.create_retries == 0 && server.deletes == 0 &&
                   server.gcp_compute_creates == 0,
               "GCP diagnostics perform no mutations");
    auto exact_catalogue_ceiling = price_spec;
    exact_catalogue_ceiling.resources.max_price_per_hour = 0.072;
    (void)gcp_priced.plan(exact_catalogue_ceiling);
    auto gcp_l4_price = price_spec;
    gcp_l4_price.resources.gpu = "l4";
    tst::check(std::fabs(*gcp_priced.plan(gcp_l4_price).estimated_hourly_cost - 0.628) < 1e-12,
               "GCP GPU catalogue price ignores custom, sole-tenancy, and commitment SKUs");
    (void)gcp_priced.plan(gcp_l4_price);
    tst::check(server.gcp_price_reads == 4, "GCP paginated price cache");

    auto gcp_bundled_disk_price = price_spec;
    gcp_bundled_disk_price.resources.gpu = "a100";
    tst::check(!gcp_priced.plan(gcp_bundled_disk_price).estimated_hourly_cost,
               "GCP bundled Local SSD shape has no incomplete catalogue quote");
    gcp_bundled_disk_price.resources.max_price_per_hour = 100;
    tst::throws<cloud::error>([&] { (void)gcp_priced.plan(gcp_bundled_disk_price); },
                              "GCP bundled Local SSD price ceiling fails closed");
    tst::check(server.gcp_price_reads == 4,
               "GCP bundled Local SSD shape skips component catalogue lookup");

    cloud::config rich_lookup;
    rich_lookup.provider = "gcp";
    rich_lookup.project = "test";
    rich_lookup.regions["gcp"] = "europe-west4";
    rich_lookup.zones["gcp"] = "europe-west4-a";
    std::optional<cloud::price_request> observed_price_request;
    rich_lookup.lookup_hourly_cost = [&](const cloud::price_request& request) {
        observed_price_request = request;
        return std::optional<double>(0.5);
    };
    cloud::client rich_priced(std::move(rich_lookup));
    auto two_t4 = price_spec;
    two_t4.resources.gpu = "t4";
    two_t4.resources.gpu_count = 2;
    two_t4.resources.spot = true;
    (void)rich_priced.plan(two_t4);
    tst::check(observed_price_request && observed_price_request->provider == "gcp" &&
                   observed_price_request->region == "europe-west4" &&
                   observed_price_request->zone == "europe-west4-a" &&
                   observed_price_request->machine_type == "n1-standard-8" &&
                   observed_price_request->accelerator == "t4" &&
                   observed_price_request->accelerator_count == 2 && observed_price_request->spot &&
                   observed_price_request->cpus == 4 && observed_price_request->memory_gb == 16,
               "rich price lookup receives native location and attached accelerators");

    cloud::config legacy_lookup;
    legacy_lookup.provider = "gcp";
    legacy_lookup.project = "test";
    legacy_lookup.estimate_hourly_cost = [](auto, auto, auto, bool) {
        return std::optional<double>(0.01);
    };
    cloud::client legacy_priced(std::move(legacy_lookup));
    tst::throws<cloud::error>([&] { (void)legacy_priced.plan(two_t4); },
                              "legacy price estimator fails closed for accelerator plans");

    cloud::config overflowing_price;
    overflowing_price.provider = "gcp";
    overflowing_price.project = "test";
    overflowing_price.lookup_hourly_cost = [](const cloud::price_request&) {
        return std::optional<double>((std::numeric_limits<double>::max)());
    };
    cloud::client overflowing_priced(std::move(overflowing_price));
    auto overflowing_spec = price_spec;
    overflowing_spec.timeout = std::chrono::hours(2);
    tst::throws<cloud::error>(
        [&] { (void)overflowing_priced.diagnose(overflowing_spec, std::chrono::hours(2)); },
        "overflowing diagnostic cost fails closed");

    cloud::config aws_prices;
    aws_prices.provider = "aws";
    aws_prices.region = "eu-west-1";
    aws_prices.allow_insecure_http = true;
    aws_prices.aws.access_key_id = "AKIDEXAMPLE";
    aws_prices.aws.secret_access_key = "secret";
    aws_prices.aws.job_queue = "cpu-queue";
    aws_prices.aws.machine_type = "m6i.xlarge";
    aws_prices.aws.pricing_endpoint = server.url();
    aws_prices.prices = cloud::price_source::public_catalogue;
    cloud::client aws_priced(std::move(aws_prices));
    price_spec.timeout = std::chrono::seconds(60);
    tst::check(aws_priced.plan(price_spec).estimated_hourly_cost == 0.42,
               "AWS built-in on-demand price");
    price_spec.retries = 2;
    const auto aws_diagnostics = aws_priced.diagnose(price_spec, std::chrono::seconds(30));
    tst::check(aws_diagnostics.provider_attempt_timeout == std::chrono::seconds(60) &&
                   !aws_diagnostics.provider_job_timeout &&
                   aws_diagnostics.configured_attempt_limit == 3 &&
                   std::any_of(aws_diagnostics.warnings.begin(), aws_diagnostics.warnings.end(),
                               [](const std::string& warning) {
                                   return warning.find("does not retry attempts terminated") !=
                                          std::string::npos;
                               }),
               "AWS diagnostic timeout and retry qualifications");
    tst::check(server.aws_registers == 0 && server.aws_submits == 0,
               "AWS diagnostics perform no mutations");

    cloud::config aws_spot_prices;
    aws_spot_prices.provider = "aws";
    aws_spot_prices.region = "eu-west-1";
    aws_spot_prices.zone = "eu-west-1a";
    aws_spot_prices.allow_insecure_http = true;
    aws_spot_prices.aws.access_key_id = "AKIDEXAMPLE";
    aws_spot_prices.aws.secret_access_key = "secret";
    aws_spot_prices.aws.spot_job_queue = "spot-queue";
    aws_spot_prices.aws.spot_machine_type = "m6i.xlarge";
    aws_spot_prices.aws.ec2_endpoint = server.url();
    aws_spot_prices.prices = cloud::price_source::public_catalogue;
    cloud::client aws_spot_priced(std::move(aws_spot_prices));
    price_spec.resources.spot = true;
    tst::check(aws_spot_priced.plan(price_spec).estimated_hourly_cost == 0.125,
               "AWS built-in Spot price");
    price_spec.resources.spot = false;

    cloud::config azure_prices;
    azure_prices.provider = "azure";
    azure_prices.region = "europe";
    azure_prices.allow_insecure_http = true;
    azure_prices.auth = cloud::auth::bearer("azure-token");
    azure_prices.azure.batch_endpoint = server.url();
    azure_prices.azure.pricing_endpoint = server.url();
    azure_prices.prices = cloud::price_source::public_catalogue;
    cloud::client azure_priced(std::move(azure_prices));
    tst::check(azure_priced.plan(price_spec).estimated_hourly_cost == 0.20,
               "Azure built-in retail price");
    const auto azure_diagnostics =
        azure_priced.diagnose(price_spec, std::chrono::seconds(30));
    tst::check(azure_diagnostics.provider_attempt_timeout == std::chrono::seconds(60) &&
                   azure_diagnostics.provider_job_timeout == std::chrono::seconds(450) &&
                   azure_diagnostics.configured_attempt_limit == 3 &&
                   std::any_of(azure_diagnostics.warnings.begin(), azure_diagnostics.warnings.end(),
                               [](const std::string& warning) {
                                   return warning.find("system recovery") != std::string::npos;
                               }),
               "Azure diagnostic watchdog and recovery qualification");
    tst::check(server.azure_jobs == 0 && server.azure_tasks == 0,
               "Azure diagnostics perform no mutations");
    price_spec.resources.spot = true;
    tst::check(azure_priced.plan(price_spec).estimated_hourly_cost == 0.04,
               "Azure built-in Spot price");
    price_spec.resources.spot = false;

    server.azure_cross_origin_price = true;
    cloud::config unsafe_azure_prices;
    unsafe_azure_prices.provider = "azure";
    unsafe_azure_prices.region = "europe";
    unsafe_azure_prices.allow_insecure_http = true;
    unsafe_azure_prices.azure.batch_endpoint = server.url();
    unsafe_azure_prices.azure.pricing_endpoint = server.url();
    unsafe_azure_prices.prices = cloud::price_source::public_catalogue;
    cloud::client unsafe_azure_priced(std::move(unsafe_azure_prices));
    tst::throws<cloud::error>([&] { (void)unsafe_azure_priced.plan(price_spec); },
                              "Azure retail pagination rejects a cross-origin URL");
    server.azure_cross_origin_price = false;

    cloud::config cheapest_config;
    cheapest_config.providers = {"gcp", "aws", "azure"};
    cheapest_config.selection = cloud::selection::lowest_cost;
    cheapest_config.aws.job_queue = "cpu-queue";
    cheapest_config.aws.machine_type = "m6i.xlarge";
    cheapest_config.azure.batch_endpoint = "https://example.invalid";
    cheapest_config.estimate_hourly_cost = [](auto provider, auto, auto, bool) {
        return std::optional<double>(provider == "azure" ? 0.1 : provider == "aws" ? 0.2 : 0.3);
    };
    cloud::client cheapest(std::move(cheapest_config));
    const auto cheapest_diagnostics =
        cheapest.diagnose(price_spec, std::chrono::seconds(30));
    tst::check(cheapest_diagnostics.selected_plan.provider == "azure" &&
                   cheapest_diagnostics.estimated_cost_for_expected_attempt_runtime &&
                   std::fabs(*cheapest_diagnostics.estimated_cost_for_expected_attempt_runtime -
                             (0.1 / 120.0)) < 1e-12,
               "lowest-cost provider selection and diagnostic cost");
    tst::throws<cloud::error>([&] { (void)cheapest.selected_provider(); },
                              "unrouted multi-provider client has no selected provider");
    tst::throws<cloud::error>([&] { (void)cheapest.storage().list("cloud://bucket"); },
                              "unrouted multi-provider storage fails closed");
    tst::throws<cloud::error>([&] { (void)cheapest.compute().instances(); },
                              "unrouted multi-provider compute fails closed");
    const auto azure_route = cheapest.route(price_spec);
    tst::check(azure_route.selected_provider() == "azure" &&
                   azure_route.plan(price_spec).provider == "azure",
               "route(job) fixes the planned provider");
    const auto aws_override = cheapest.route("aws");
    tst::check(aws_override.selected_provider() == "aws" &&
                   aws_override.plan(price_spec).provider == "aws",
               "route(provider) overrides cheapest routing locally");
    tst::throws<cloud::error>([&] { (void)cheapest.route("unknown"); },
                              "explicit route rejects an unknown provider");
}

void environment_factory_tests() {
    environment_guard environment{
        "CLOUD_REGION",
        "CLOUD_ZONE",
        "CLOUD_GCP_PROJECT",
        "CLOUD_GCP_REGION",
        "CLOUD_GCP_ZONE",
        "CLOUD_COMPUTE_TEMPLATE",
        "CLOUD_GCP_INSTANCE_TEMPLATE",
        "CLOUD_AWS_JOB_QUEUE",
        "CLOUD_AWS_SPOT_JOB_QUEUE",
        "CLOUD_AWS_FARGATE_JOB_QUEUE",
        "CLOUD_AWS_FARGATE_SPOT_JOB_QUEUE",
        "CLOUD_AWS_EXECUTION_ROLE_ARN",
        "CLOUD_AWS_JOB_ROLE_ARN",
        "CLOUD_AWS_S3_FILES_BUCKET",
        "CLOUD_AWS_S3_FILES_FILE_SYSTEM_ARN",
        "CLOUD_AWS_S3_FILES_ACCESS_POINT_ARN",
        "CLOUD_AWS_S3_FILES_INPUT_BUCKET",
        "CLOUD_AWS_S3_FILES_INPUT_FILE_SYSTEM_ARN",
        "CLOUD_AWS_S3_FILES_INPUT_ACCESS_POINT_ARN",
        "CLOUD_AWS_S3_FILES_OUTPUT_BUCKET",
        "CLOUD_AWS_S3_FILES_OUTPUT_FILE_SYSTEM_ARN",
        "CLOUD_AWS_S3_FILES_OUTPUT_ACCESS_POINT_ARN",
        "CLOUD_AWS_LAUNCH_TEMPLATE_ID",
        "CLOUD_AWS_LAUNCH_TEMPLATE_NAME",
        "CLOUD_AWS_LAUNCH_TEMPLATE_VERSION",
        "CLOUD_AWS_MACHINE_TYPE",
        "CLOUD_AWS_SPOT_MACHINE_TYPE",
        "CLOUD_AWS_LOG_GROUP",
        "CLOUD_AWS_REGION",
        "CLOUD_AWS_ZONE",
        "CLOUD_AWS_GPU_MODEL",
        "CLOUD_AWS_GPU_JOB_QUEUE",
        "CLOUD_AWS_GPU_SPOT_JOB_QUEUE",
        "CLOUD_AWS_GPU_MACHINE_TYPE",
        "CLOUD_AWS_GPU_CPUS",
        "CLOUD_AWS_GPU_MEMORY_GB",
        "CLOUD_AWS_GPU_COUNT",
        "AWS_ACCESS_KEY_ID",
        "AWS_SECRET_ACCESS_KEY",
        "AWS_SESSION_TOKEN",
        "CLOUD_AZURE_BATCH_ENDPOINT",
        "CLOUD_AZURE_REGION",
        "CLOUD_AZURE_BATCH_TOKEN",
        "CLOUD_AZURE_STORAGE_ACCOUNT",
        "CLOUD_AZURE_STORAGE_TOKEN",
        "CLOUD_AZURE_STORAGE_SAS",
        "CLOUD_AZURE_SUBSCRIPTION_ID",
        "CLOUD_AZURE_RESOURCE_GROUP",
        "CLOUD_AZURE_MANAGEMENT_TOKEN",
        "CLOUD_AZURE_VM_IMAGE_ID",
        "CLOUD_AZURE_VM_SUBNET_ID",
        "CLOUD_AZURE_VM_SIZE",
        "CLOUD_AZURE_VM_LOCATION",
        "CLOUD_AZURE_VM_OS_DISK_TYPE",
    };

    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("unknown"); },
                              "environment factory rejects an unknown provider");
    tst::throws<cloud::error>(
        [] {
            (void)cloud::client::from_environment("cheapest", cloud::price_source::none);
        },
        "cheapest environment factory rejects disabled pricing");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("gcp"); },
                              "environment factory requires a GCP project");
    const auto storage_only_aws = cloud::client::from_environment("aws");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("azure"); },
                              "environment factory requires an Azure endpoint");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("cheapest"); },
                              "cheapest routing rejects an empty environment");
    environment.set("CLOUD_GCP_PROJECT", "bad/project");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("gcp"); },
                              "environment factory validates the GCP project");
    environment.unset("CLOUD_GCP_PROJECT");

    cloud::job_spec spec;
    spec.image = "image";
    spec.command = {"run"};
    spec.resources.cpus = 4;
    spec.resources.memory_gb = 16;
    tst::throws<cloud::error>([&] { (void)storage_only_aws.plan(spec); },
                              "AWS job planning still requires a Batch queue");

    environment.set("CLOUD_REGION", "us");
    environment.set("CLOUD_ZONE", "shared-zone");
    environment.set("CLOUD_GCP_PROJECT", "test-project");
    environment.set("CLOUD_COMPUTE_TEMPLATE", "worker");
    environment.set("CLOUD_GCP_INSTANCE_TEMPLATE", "gcp-worker-template");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("cheapest"); },
                              "cheapest routing requires at least two providers");
    environment.set("CLOUD_GCP_REGION", "europe-west4");
    environment.set("CLOUD_GCP_ZONE", "europe-west4-a");

    const cloud::config gcp_config = cloud::detail::config_from_environment("gcp");
    tst::check(gcp_config.provider && *gcp_config.provider == "gcp" &&
                   gcp_config.project == "test-project" && gcp_config.region == "us" &&
                   gcp_config.zone == "shared-zone" &&
                   cloud::detail::configured_region(gcp_config, "gcp") == "europe-west4" &&
                   cloud::detail::configured_zone(gcp_config, "gcp") == "europe-west4-a" &&
                   gcp_config.instance_templates.at("worker").gcp_instance_template ==
                       "gcp-worker-template",
               "GCP environment mapping");
    const auto gcp_plan = cloud::client::from_environment("gcp").plan(spec);
    tst::check(gcp_plan.provider == "gcp" && gcp_plan.region == "europe-west4" &&
                   gcp_plan.machine_type == "e2-standard-4" && !gcp_plan.estimated_hourly_cost,
               "GCP environment client plans locally");

    environment.set("CLOUD_AWS_JOB_QUEUE", "cpu-queue");
    environment.set("CLOUD_AWS_SPOT_JOB_QUEUE", "cpu-spot-queue");
    environment.set("CLOUD_AWS_MACHINE_TYPE", "m6i.xlarge");
    environment.set("CLOUD_AWS_SPOT_MACHINE_TYPE", "m6i.xlarge");
    environment.set("CLOUD_AWS_LOG_GROUP", "/aws/batch/cloud-test");
    environment.set("CLOUD_AWS_REGION", "eu-west-1");
    environment.set("CLOUD_AWS_ZONE", "eu-west-1a");
    environment.set("CLOUD_AWS_FARGATE_JOB_QUEUE", "fargate-queue");
    environment.set("CLOUD_AWS_EXECUTION_ROLE_ARN", "arn:aws:iam::123:role/execution");
    environment.set("CLOUD_AWS_JOB_ROLE_ARN", "arn:aws:iam::123:role/job");
    environment.set("CLOUD_AWS_S3_FILES_BUCKET", "inputs");
    environment.set("CLOUD_AWS_S3_FILES_FILE_SYSTEM_ARN",
                    "arn:aws:s3files:eu-west-1:123:file-system/fs-1");
    environment.set("CLOUD_AWS_S3_FILES_INPUT_BUCKET", "dispatch-input");
    environment.set("CLOUD_AWS_S3_FILES_INPUT_FILE_SYSTEM_ARN",
                    "arn:aws:s3files:eu-west-1:123:file-system/fs-input");
    environment.set("CLOUD_AWS_S3_FILES_OUTPUT_BUCKET", "dispatch-output");
    environment.set("CLOUD_AWS_S3_FILES_OUTPUT_FILE_SYSTEM_ARN",
                    "arn:aws:s3files:eu-west-1:123:file-system/fs-output");
    environment.set("CLOUD_AWS_LAUNCH_TEMPLATE_ID", "lt-123");

    const cloud::config aws_config = cloud::detail::config_from_environment("aws");
    tst::check(aws_config.provider && *aws_config.provider == "aws" &&
                   aws_config.aws.job_queue == "cpu-queue" &&
                   aws_config.aws.spot_job_queue == "cpu-spot-queue" &&
                   aws_config.aws.machine_type == "m6i.xlarge" &&
                   aws_config.aws.spot_machine_type == "m6i.xlarge" &&
                   aws_config.aws.log_group == "/aws/batch/cloud-test" &&
                   aws_config.aws.fargate_job_queue == "fargate-queue" &&
                   aws_config.aws.s3_files.at("inputs").file_system_arn.find("fs-1") !=
                       std::string::npos &&
                   aws_config.aws.s3_files.at("dispatch-input").file_system_arn.find(
                       "fs-input") !=
                       std::string::npos &&
                   aws_config.aws.s3_files.at("dispatch-output").file_system_arn.find(
                       "fs-output") !=
                       std::string::npos &&
                   aws_config.instance_templates.at("worker").aws.id == "lt-123" &&
                   cloud::detail::configured_region(aws_config, "aws") == "eu-west-1" &&
                   cloud::detail::configured_zone(aws_config, "aws") == "eu-west-1a",
               "AWS environment mapping");
    environment.set("CLOUD_AWS_S3_FILES_OUTPUT_BUCKET", "inputs");
    tst::throws<cloud::error>([] { (void)cloud::detail::config_from_environment("aws"); },
                              "AWS environment rejects conflicting named mount mappings");
    environment.set("CLOUD_AWS_S3_FILES_OUTPUT_BUCKET", "dispatch-output");
    const auto aws_plan = cloud::client::from_environment("aws").plan(spec);
    tst::check(aws_plan.provider == "aws" && aws_plan.region == "eu-west-1" &&
                   aws_plan.machine_type == "m6i.xlarge" && !aws_plan.estimated_hourly_cost,
               "AWS environment client plans locally");

    environment.unset("CLOUD_AWS_JOB_QUEUE");
    environment.unset("CLOUD_AWS_MACHINE_TYPE");
    auto spot_spec = spec;
    spot_spec.resources.spot = true;
    const auto spot_plan = cloud::client::from_environment("aws").plan(spot_spec);
    tst::check(spot_plan.machine_type == "m6i.xlarge" && spot_plan.region == "eu-west-1",
               "AWS environment permits a Spot-only route");
    environment.set("CLOUD_AWS_JOB_QUEUE", "cpu-queue");
    environment.set("CLOUD_AWS_MACHINE_TYPE", "m6i.xlarge");

    environment.set("CLOUD_AZURE_REGION", "westeurope");
    environment.set("CLOUD_AZURE_BATCH_ENDPOINT", "https://evil.example");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("azure"); },
                              "Azure environment rejects an arbitrary token destination");
    environment.set("CLOUD_AZURE_BATCH_ENDPOINT", "https://test.westeurope.batch.azure.com/jobs");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("azure"); },
                              "Azure environment rejects an endpoint path");
    environment.set("CLOUD_AZURE_BATCH_ENDPOINT", "https://test.westeurope.batch.azure.com");
    environment.set("CLOUD_AZURE_STORAGE_ACCOUNT", "teststorage");
    environment.set("CLOUD_AZURE_STORAGE_SAS", "?sv=test&sig=secret");
    environment.set("CLOUD_AZURE_SUBSCRIPTION_ID", "subscription");
    environment.set("CLOUD_AZURE_RESOURCE_GROUP", "workers");
    environment.set("CLOUD_AZURE_VM_IMAGE_ID", "/subscriptions/example/images/worker");
    environment.set("CLOUD_AZURE_VM_SUBNET_ID", "/subscriptions/example/subnets/workers");
    environment.set("CLOUD_AZURE_VM_SIZE", "Standard_D4s_v5");
    environment.set("CLOUD_AZURE_VM_LOCATION", "westeurope");

    const cloud::config azure_config = cloud::detail::config_from_environment("azure");
    tst::check(azure_config.provider && *azure_config.provider == "azure" &&
                   azure_config.azure.batch_endpoint == "https://test.westeurope.batch.azure.com" &&
                   azure_config.azure.storage_account == "teststorage" &&
                   azure_config.azure.storage_sas == "sv=test&sig=secret" &&
                   azure_config.instance_templates.at("worker").azure.machine_type ==
                       "Standard_D4s_v5" &&
                   azure_config.azure.auth &&
                   cloud::detail::configured_region(azure_config, "azure") == "westeurope",
               "Azure environment mapping");
    const auto azure_plan = cloud::client::from_environment("azure").plan(spec);
    tst::check(azure_plan.provider == "azure" && azure_plan.region == "westeurope" &&
                   azure_plan.machine_type == "Standard_D4s_v5" &&
                   !azure_plan.estimated_hourly_cost,
               "Azure environment client plans locally");

    environment.set("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
    environment.set("AWS_SECRET_ACCESS_KEY", "secret");
    cloud::config cheapest_config = cloud::detail::config_from_environment("cheapest");
    tst::check(cheapest_config.providers == std::vector<cloud::provider>{"gcp", "aws", "azure"} &&
                   cheapest_config.selection == cloud::selection::lowest_cost &&
                   cheapest_config.prices == cloud::price_source::public_catalogue,
               "cheapest environment uses stable provider order");

    std::vector<cloud::provider> compared;
    cheapest_config.lookup_hourly_cost = [&](const cloud::price_request& request) {
        compared.push_back(request.provider);
        return std::optional<double>(request.provider == "azure" ? 0.10
                                     : request.provider == "aws" ? 0.20
                                                                 : 0.30);
    };
    const auto cheapest_plan = cloud::client(std::move(cheapest_config)).plan(spec);
    tst::check(compared == std::vector<cloud::provider>{"gcp", "aws", "azure"} &&
                   cheapest_plan.provider == "azure" && cheapest_plan.estimated_hourly_cost == 0.10,
               "cheapest environment compares without a network request");

    cloud::config tied_config = cloud::detail::config_from_environment("cheapest");
    tied_config.lookup_hourly_cost = [](const cloud::price_request&) {
        return std::optional<double>(0.25);
    };
    tst::check(cloud::client(std::move(tied_config)).plan(spec).provider == "gcp",
               "cheapest price ties follow configured order");

    cloud::config missing_quote = cloud::detail::config_from_environment("cheapest");
    missing_quote.lookup_hourly_cost = [](const cloud::price_request& request) {
        return request.provider == "aws" ? std::nullopt : std::optional<double>(0.25);
    };
    tst::throws<cloud::error>([&] { (void)cloud::client(missing_quote).plan(spec); },
                              "cheapest routing rejects a partial quote set");

    cloud::config failed_provider = cloud::detail::config_from_environment("cheapest");
    failed_provider.lookup_hourly_cost = [](const cloud::price_request& request) {
        if (request.provider == "azure")
            throw cloud::error("price provider failed");
        return std::optional<double>(0.25);
    };
    tst::throws<cloud::error>([&] { (void)cloud::client(failed_provider).plan(spec); },
                              "cheapest routing rejects a failed provider");

    environment.set("CLOUD_AWS_GPU_MODEL", "nvidia-l4");
    environment.set("CLOUD_AWS_GPU_JOB_QUEUE", "l4-queue");
    environment.set("CLOUD_AWS_GPU_SPOT_JOB_QUEUE", "l4-spot-queue");
    environment.set("CLOUD_AWS_GPU_MACHINE_TYPE", "g6.xlarge");
    environment.set("CLOUD_AWS_GPU_CPUS", "4");
    environment.set("CLOUD_AWS_GPU_MEMORY_GB", "16");
    environment.set("CLOUD_AWS_GPU_COUNT", "1");
    environment.unset("CLOUD_AWS_JOB_QUEUE");
    environment.unset("CLOUD_AWS_SPOT_JOB_QUEUE");
    environment.unset("CLOUD_AWS_MACHINE_TYPE");
    environment.unset("CLOUD_AWS_SPOT_MACHINE_TYPE");

    const cloud::config gpu_config = cloud::detail::config_from_environment("aws");
    const auto target = gpu_config.aws.gpu_targets.find("l4");
    tst::check(target != gpu_config.aws.gpu_targets.end() &&
                   target->second.job_queue == "l4-queue" &&
                   target->second.spot_job_queue == "l4-spot-queue" &&
                   target->second.machine_type == "g6.xlarge" && target->second.cpus == 4 &&
                   target->second.memory_gb == 16 && target->second.gpus == 1,
               "complete AWS GPU environment mapping");
    auto gpu_spec = spec;
    gpu_spec.resources.gpu = "L4";
    const auto gpu_plan = cloud::client::from_environment("aws").plan(gpu_spec);
    tst::check(gpu_plan.machine_type == "g6.xlarge" && gpu_plan.accelerator == "l4",
               "AWS GPU-only environment client planning");

    environment.unset("CLOUD_AWS_GPU_MEMORY_GB");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("aws"); },
                              "partial AWS GPU environment is rejected");
    environment.set("CLOUD_AWS_GPU_MEMORY_GB", "16");
    environment.set("CLOUD_AWS_GPU_CPUS", "4x");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("aws"); },
                              "malformed AWS GPU integer is rejected");
    environment.set("CLOUD_AWS_GPU_CPUS", "4");
    environment.set("CLOUD_AWS_GPU_MEMORY_GB", "not-a-number");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("aws"); },
                              "malformed AWS GPU decimal is rejected");

    const auto scientific = cloud::detail::parse_decimal("1.25e2");
    tst::check(scientific && *scientific == 125.0, "portable decimal exponent parsing");
    for (const std::string_view invalid : {"", "+1", "1e", "1x", "nan", "inf", "1e-9999"})
        tst::check(!cloud::detail::parse_decimal(invalid),
                   "malformed decimal is rejected: " + std::string(invalid));
}

void planning_tests() {
    static_assert(cloud::gcp::version == CLOUD_H_VERSION);
    static_assert(CLOUD_H_VERSION_NUM == 0x000400);
    static_assert(std::is_aggregate_v<cloud::resources>);
    static_assert(std::is_aggregate_v<cloud::job_spec>);
    static_assert(std::is_aggregate_v<cloud::run_diagnostics>);
    static_assert(std::is_aggregate_v<cloud::command_record>);
    static_assert(!std::is_aggregate_v<cloud::command_output>);

    cloud::config config;
    config.project = "test-project";
    config.region = "europe";
    config.auth = cloud::auth::bearer("test-token");
    config.lookup_hourly_cost = [](const cloud::price_request& request) {
        tst::check(request.machine_type == "e2-standard-4" ||
                       request.machine_type == "g2-standard-4",
                   "price machine");
        tst::check(request.spot, "price spot");
        return std::optional<double>(0.24);
    };
    cloud::client client(std::move(config));

    cloud::job_spec spec;
    spec.name = "Analysis 42";
    spec.image = "python:3.13";
    spec.command = {"python", "analyse.py", "/input/input.csv"};
    spec.workdir = ".";
    spec.service_account = "batch-runner@test-project.iam.gserviceaccount.com";
    spec.mounts = {
        {"cloud://inputs/run-42/", "/input", true},
        {"cloud://results", "/output"},
    };
    spec.resources.cpus = 4;
    spec.resources.memory_gb = 16;
    spec.resources.spot = true;
    spec.resources.max_price_per_hour = 0.50;
    spec.retries = 2;
    spec.auto_delete = true;
    spec.timeout = std::chrono::hours(2);

    const auto plan = client.plan(spec);
    tst::check(plan.provider == "gcp", "provider");
    tst::check(plan.region == "europe-west4", "region alias");
    tst::check(plan.machine_type == "e2-standard-4", "machine mapping");
    tst::check(plan.estimated_hourly_cost == 0.24, "price estimate");
    tst::check(client.supports(cloud::feature::spot_instances), "spot support");
    tst::check(client.supports(cloud::feature::accelerators), "gpu support");

    const cloud::run_diagnostics diagnostics =
        client.diagnose(spec, std::chrono::minutes(30));
    tst::check(diagnostics.selected_plan.provider == "gcp" &&
                   diagnostics.expected_attempt_runtime == std::chrono::minutes(30) &&
                   diagnostics.controller_timeout == std::chrono::hours(2),
               "diagnostic plan and caller runtime");
    tst::check(diagnostics.provider_attempt_timeout == std::chrono::seconds(7'200) &&
                   !diagnostics.provider_job_timeout && diagnostics.configured_retries == 2 &&
                   diagnostics.configured_attempt_limit == 3,
               "diagnostic timeout and retry facts");
    tst::check(diagnostics.estimated_cost_for_expected_attempt_runtime &&
                   std::fabs(*diagnostics.estimated_cost_for_expected_attempt_runtime - 0.12) <
                       1e-12,
               "diagnostic compute-cost sensitivity");
    tst::check(std::any_of(diagnostics.warnings.begin(), diagnostics.warnings.end(),
                           [](const std::string& warning) {
                               return warning.find("caller-supplied") != std::string::npos;
                           }) &&
                   std::any_of(diagnostics.warnings.begin(), diagnostics.warnings.end(),
                               [](const std::string& warning) {
                                   return warning.find("egress cost") != std::string::npos;
                               }),
               "diagnostic warnings include plan and runtime qualifications");

    tst::throws<cloud::error>(
        [&] { (void)client.diagnose(spec, std::chrono::milliseconds::zero()); },
        "zero expected attempt runtime is rejected");
    tst::throws<cloud::error>(
        [&] { (void)client.diagnose(spec, std::chrono::hours(3)); },
        "expected attempt runtime beyond controller timeout is rejected");

    cloud::config unpriced_config;
    unpriced_config.project = "test-project";
    unpriced_config.region = "europe";
    unpriced_config.auth = cloud::auth::bearer("test-token");
    unpriced_config.lookup_hourly_cost =
        [](const cloud::price_request&) { return std::optional<double>{}; };
    cloud::client unpriced(std::move(unpriced_config));
    cloud::job_spec unpriced_spec = spec;
    unpriced_spec.resources.max_price_per_hour.reset();
    const cloud::run_diagnostics unavailable =
        unpriced.diagnose(unpriced_spec, std::chrono::minutes(30));
    tst::check(!unavailable.selected_plan.estimated_hourly_cost &&
                   !unavailable.estimated_cost_for_expected_attempt_runtime,
               "unavailable prices remain unavailable in diagnostics");

    const std::string body = cloud::detail::batch_body(spec, plan);
    const auto batch = cloud::gcp::detail::parse_json(body);
    const auto* groups = batch.get("taskGroups");
    tst::check(groups && groups->array().size() == 1, "Batch task group");
    tst::check(body.find("\"entrypoint\":\"python\"") != std::string::npos,
               "direct argv entrypoint");
    tst::check(body.find("\"commands\":[\"analyse.py\",\"/input/input.csv\"]") != std::string::npos,
               "direct argv");
    tst::check(body.find("\"cpuMilli\":\"4000\"") != std::string::npos, "cpu");
    tst::check(body.find("\"memoryMib\":\"16384\"") != std::string::npos, "memory");
    tst::check(body.find("\"maxRetryCount\":2") != std::string::npos, "retries");
    tst::check(body.find("\"provisioningModel\":\"SPOT\"") != std::string::npos, "spot");
    tst::check(body.find("batch-runner@test-project.iam.gserviceaccount.com") != std::string::npos,
               "service account");
    tst::check(body.find("/mnt/disks/cloud-0:/input:ro") != std::string::npos, "read-only mount");
    tst::check(body.find("\"remotePath\":\"inputs/run-42/\"") != std::string::npos, "GCS prefix");
    tst::check(body.find("\"cloud-hpp\":\"temporary\"") != std::string::npos,
               "stable provider resource label");

    const auto uri = cloud::detail::parse_uri("cloud://bucket/path/to/object");
    tst::check(uri.bucket == "bucket" && uri.key == "path/to/object", "cloud URI");
    tst::check(cloud::gcp::detail::crc32c("123456789") == "4waSgw==", "CRC32C");
    tst::check(cloud::gcp::detail::field(cloud::gcp::detail::parse_json("{\"x\":\"\\u03bb\"}"),
                                         "x") == "\xce\xbb",
               "JSON unicode");
    const auto json = cloud::gcp::detail::parse_json("{\"items\":[{},{}],\"number\":-1.25e+2}");
    tst::check(json.get("items") && json.get("items")->array().size() == 2 &&
                   cloud::gcp::detail::field(json, "number") == "-1.25e+2",
               "templated JSON collections and numbers");
    tst::throws<cloud::error>([] { (void)cloud::gcp::detail::parse_json("[\v]"); },
                              "JSON whitespace is locale independent");
    for (const std::string_view invalid : {"1.", "1e", "-"})
        tst::throws<cloud::error>([=] { (void)cloud::gcp::detail::parse_json(invalid); },
                                  "malformed JSON number rejected");
    cloud::gcp::detail::JsonLimits depth_limit;
    depth_limit.max_depth = 2;
    (void)cloud::gcp::detail::parse_json("[[0]]", depth_limit);
    tst::throws<cloud::error>(
        [&] { (void)cloud::gcp::detail::parse_json("[[[0]]]", depth_limit); },
        "JSON nesting limit");
    cloud::gcp::detail::JsonLimits node_limit;
    node_limit.max_nodes = 3;
    (void)cloud::gcp::detail::parse_json("[0,1]", node_limit);
    tst::throws<cloud::error>(
        [&] { (void)cloud::gcp::detail::parse_json("[0,1,2]", node_limit); },
        "JSON node limit");
    cloud::gcp::detail::JsonLimits byte_limit;
    byte_limit.max_bytes = 3;
    (void)cloud::gcp::detail::parse_json("[0]", byte_limit);
    byte_limit.max_bytes = 2;
    tst::throws<cloud::error>([&] { (void)cloud::gcp::detail::parse_json("[0]", byte_limit); },
                              "JSON byte limit");
    tst::throws<cloud::error>(
        [] {
            (void)cloud::gcp::detail::unsigned_field(
                cloud::gcp::detail::parse_json("{\"value\":\"42suffix\"}"), "value");
        },
        "unsigned provider field requires complete input consumption");
    tst::check(!cloud::gcp::detail::is_ascii_alnum(static_cast<char>(0xe5)),
               "identifier grammar is ASCII");
    tst::check(cloud::detail::parse_state("QUEUED") == cloud::job_state::queued &&
                   cloud::detail::parse_state("SCHEDULED") == cloud::job_state::scheduled &&
                   cloud::detail::parse_state("RUNNING") == cloud::job_state::running &&
                   cloud::detail::parse_state("SUCCEEDED") == cloud::job_state::succeeded &&
                   cloud::detail::parse_state("FAILED") == cloud::job_state::failed &&
                   cloud::detail::parse_state("CANCELLATION_IN_PROGRESS") ==
                       cloud::job_state::cancelling &&
                   cloud::detail::parse_state("CANCELLED") == cloud::job_state::cancelled &&
                   cloud::detail::parse_state("DELETION_IN_PROGRESS") ==
                       cloud::job_state::deleting &&
                   cloud::detail::parse_state("NOT_A_STATE") == cloud::job_state::unknown,
               "Batch state table");

    cloud::detail::job_data azure_cancel_race;
    azure_cancel_race.chosen.provider = "azure";
    azure_cancel_race.cancel_requested = true;
    tst::check(cloud::detail::update(
                   azure_cancel_race,
                   cloud::gcp::detail::parse_json(
                       "{\"state\":\"completed\",\"executionInfo\":{\"result\":\"success\"}}")) ==
                   cloud::job_state::succeeded,
               "Azure completion wins a cancellation race");

    const std::string id = cloud::detail::job_id("42 / VERY Long Job Name !!!");
    tst::check(id.size() <= 63 && std::isalpha(static_cast<unsigned char>(id.front())),
               "Batch job id");
    for (const char raw : id) {
        const auto c = static_cast<unsigned char>(raw);
        tst::check(std::islower(c) || std::isdigit(c) || c == '-', "Batch job id chars");
    }

    auto too_expensive = spec;
    too_expensive.resources.max_price_per_hour = 0.10;
    tst::throws<cloud::error>([&] { (void)client.plan(too_expensive); }, "price ceiling");

    auto gpu = spec;
    gpu.resources.gpu = "L4";
    const auto gpu_plan = client.plan(gpu);
    tst::check(gpu_plan.machine_type == "g2-standard-4" && gpu_plan.accelerator == "l4" &&
                   gpu_plan.accelerator_count == 1,
               "GCP L4 mapping");
    tst::check(cloud::detail::batch_body(gpu, gpu_plan).find("installGpuDrivers") !=
                   std::string::npos,
               "GCP GPU driver policy");

    auto invalid_price = spec;
    invalid_price.resources.max_price_per_hour = std::numeric_limits<double>::quiet_NaN();
    tst::throws<cloud::error>([&] { (void)client.plan(invalid_price); }, "invalid price ceiling");

    auto invalid_job = spec;
    invalid_job.command.clear();
    tst::throws<cloud::error>([&] { (void)client.plan(invalid_job); }, "plan validates command");
    invalid_job = spec;
    invalid_job.retries = 11;
    tst::throws<cloud::error>([&] { (void)client.plan(invalid_job); }, "plan validates retries");
    invalid_job = spec;
    invalid_job.mounts.front().source = "cloud://inputs/one-object";
    tst::throws<cloud::error>([&] { (void)client.plan(invalid_job); }, "prefix-only mounts");

    cloud::config unsupported;
    unsupported.provider = "aws";
    unsupported.project = "test";
    unsupported.auth = cloud::auth::bearer("test");
    cloud::client aws(std::move(unsupported));
    tst::throws<cloud::error>([&] { (void)aws.plan(spec); }, "unsupported AWS");
    tst::throws<cloud::error>([&] { (void)aws.storage().list("cloud://bucket"); },
                              "unsupported AWS storage");

    cloud::config automatic;
    automatic.project = "test";
    automatic.selection = cloud::selection::lowest_cost;
    automatic.auth = cloud::auth::bearer("test");
    cloud::client cheapest(std::move(automatic));
    tst::throws<cloud::error>([&] { (void)cheapest.plan(spec); }, "unsupported lowest cost");

    cloud::config unsafe;
    unsafe.project = "test";
    unsafe.region = "europe-west4\"}";
    unsafe.auth = cloud::auth::bearer("test");
    cloud::client unsafe_region(std::move(unsafe));
    tst::throws<cloud::error>([&] { (void)unsafe_region.plan(spec); }, "region validation");
}

void command_output_tests() {
    std::string controls = "line\nwith\\control\r\t";
    controls.push_back('\0');
    controls.push_back('\x1f');
    controls.push_back('\x7f');

    std::string rendered_text;
    {
        global_locale_guard locale_scope(
            std::locale(std::locale::classic(), new comma_decimal_point));
        cloud::command_output basic;
        basic.add_number("hourly_rate_estimate_usd", 0.24)
            .add_duration_seconds("runtime_seconds", std::chrono::milliseconds(1'500))
            .add_boolean("spot", true)
            .add("warning", controls)
            .add("warning", "second");
        std::ostringstream rendered;
        rendered << basic;
        rendered_text = rendered.str();
    }
    tst::check(rendered_text ==
                   "hourly_rate_estimate_usd=0.24\n"
                   "runtime_seconds=1.5\n"
                   "spot=true\n"
                   "warning=line\\nwith\\\\control\\r\\t\\x00\\x1f\\x7f\n"
                   "warning=second\n",
               "command output is ordered, locale-stable, and escaped");

    cloud::command_output modified;
    modified.add("provider", "gcp")
        .add("warning", "one")
        .add("provider", "aws")
        .add("warning", "two")
        .set("provider", "azure");
    const std::string_view stored_key = modified.records()[1].key;
    modified.rename(stored_key, "notice")
        .erase("notice")
        .set("status", "ready");
    tst::check(modified.records().size() == 2 && modified.records()[0].key == "provider" &&
                   modified.records()[0].value == "azure" &&
                   modified.records()[1].key == "status" &&
                   modified.records()[1].value == "ready",
               "command records can be replaced, renamed, erased, and inspected");

    std::ostringstream one_record;
    cloud::write_command_record(one_record, "UPPER_2", "a=b c\n");
    tst::check(one_record.str() == "UPPER_2=a=b c\\n\n",
               "one live command record uses the same syntax");

    std::ostringstream pending_width;
    pending_width << std::setfill('.') << std::setw(20);
    cloud::write_command_record(pending_width, "key", "value");
    tst::check(pending_width.str() == "key=value\n",
               "stream formatting cannot pad a command record");

    for (const std::string_view invalid : {"", "1bad", "bad-key", "space key"})
        tst::throws<cloud::error>(
            [=] {
                cloud::command_output value;
                value.add(invalid, "value");
            },
            "command output rejects malformed keys");
    tst::throws<cloud::error>(
        [] {
            cloud::command_output value;
            value.add("valid", "value").rename("valid", "not.valid");
        },
        "command output rejects a malformed replacement key");
    tst::throws<cloud::error>(
        [] {
            cloud::command_output value;
            value.add_number("value", std::numeric_limits<double>::infinity());
        },
        "command output rejects non-finite numbers");

    cloud::job_spec job;
    job.name = "job\n42";
    job.resources.cpus = 4;
    job.resources.memory_gb = 16.5;
    job.resources.gpu = "a10";
    job.resources.gpu_count = 1;
    job.resources.spot = true;

    cloud::run_diagnostics report;
    report.selected_plan.provider = "aws";
    report.selected_plan.region = "eu-west-1";
    report.selected_plan.machine_type = "g5.xlarge";
    report.selected_plan.accelerator = "a10";
    report.selected_plan.accelerator_count = 1;
    report.selected_plan.estimated_hourly_cost = 0.24;
    report.expected_attempt_runtime = std::chrono::seconds(90);
    report.controller_timeout = std::chrono::milliseconds(1'500);
    report.provider_attempt_timeout = std::chrono::seconds(2);
    report.configured_retries = 2;
    report.configured_attempt_limit = 3;
    report.estimated_cost_for_expected_attempt_runtime = 0.006;
    report.warnings = {"first", "second\nline"};

    cloud::command_output diagnostics =
        cloud::command_output::diagnostics("cheapest", job, report);
    diagnostics.add("program", "test");
    std::ostringstream diagnostic_text;
    diagnostics.write(diagnostic_text);
    tst::check(diagnostic_text.str() ==
                   "output_version=1\n"
                   "requested_provider=cheapest\n"
                   "job_name=job\\n42\n"
                   "provider=aws\n"
                   "region=eu-west-1\n"
                   "machine=g5.xlarge\n"
                   "requested_cpus=4\n"
                   "requested_memory_gb=16.5\n"
                   "accelerator=a10\n"
                   "accelerator_count=1\n"
                   "spot=true\n"
                   "expected_attempt_runtime_seconds=90\n"
                   "controller_timeout_seconds=1.5\n"
                   "provider_attempt_timeout_seconds=2\n"
                   "provider_job_timeout_seconds=not-applicable\n"
                   "configured_retries=2\n"
                   "configured_attempt_limit=3\n"
                   "cost_currency=USD\n"
                   "hourly_rate_estimate_usd=0.24\n"
                   "estimated_cost_for_expected_attempt_runtime_usd=0.006\n"
                   "estimate_basis=expected-attempt-runtime-times-hourly-rate\n"
                   "warning=first\n"
                   "warning=second\\nline\n"
                   "preflight=planned\n"
                   "program=test\n",
               "standard diagnostics are complete and customisable");

    report.provider_job_timeout = std::chrono::seconds(123);
    report.selected_plan.estimated_hourly_cost.reset();
    report.estimated_cost_for_expected_attempt_runtime.reset();
    std::ostringstream unavailable;
    cloud::command_output::diagnostics("azure", job, report).write(unavailable);
    tst::check(unavailable.str().find("provider_job_timeout_seconds=123\n") !=
                       std::string::npos &&
                   unavailable.str().find("hourly_rate_estimate_usd=unavailable\n") !=
                       std::string::npos &&
                   unavailable.str().find(
                       "estimated_cost_for_expected_attempt_runtime_usd=unavailable\n") !=
                       std::string::npos,
               "diagnostics distinguish watchdogs and unavailable prices");

    const std::array<std::pair<cloud::job_state, std::string_view>, 10> states{{
        {cloud::job_state::queued, "queued"},
        {cloud::job_state::scheduled, "scheduled"},
        {cloud::job_state::running, "running"},
        {cloud::job_state::succeeded, "succeeded"},
        {cloud::job_state::failed, "failed"},
        {cloud::job_state::cancelling, "cancelling"},
        {cloud::job_state::cancelled, "cancelled"},
        {cloud::job_state::deleting, "deleting"},
        {cloud::job_state::unknown, "unknown"},
        {static_cast<cloud::job_state>(999), "unknown"},
    }};
    for (const auto& state : states) {
        cloud::result value;
        value.state = state.first;
        std::ostringstream state_text;
        cloud::command_output::job_result(value).write(state_text);
        tst::check(state_text.str() == "job_state=" + std::string(state.second) + "\n",
                   "command result normalises every job state");
    }

    cloud::result failed;
    failed.state = cloud::job_state::failed;
    failed.exit_code = 17;
    failed.message = "kept for stderr";
    failed.warnings = {"one", "two"};
    std::ostringstream result_text;
    cloud::command_output::job_result(failed).write(result_text);
    tst::check(result_text.str() ==
                   "job_state=failed\n"
                   "exit_code=17\n"
                   "warning=one\n"
                   "warning=two\n",
               "command result preserves warnings and leaves errors to the caller");
}

} // namespace

int main() {
    return tst::run(
        TST_CASE("plans and validates GCP workloads", planning_tests()),
        TST_CASE("bounds private provider responses", transport_limit_tests()),
        TST_CASE("constructs clients from the environment", environment_factory_tests()),
        TST_CASE("runs GCP lifecycle, storage, and compute", gcp_lifecycle_tests()),
        TST_CASE("maps the storage facade to AWS S3", aws_storage_tests()),
        TST_CASE("maps the storage facade to Azure Blob", azure_storage_tests()),
        TST_CASE("binds storage to an explicit provider route", storage_route_tests()),
        TST_CASE("controls raw AWS EC2 instances", aws_compute_tests()),
        TST_CASE("controls raw Azure virtual machines", azure_compute_tests()),
        TST_CASE("submits AWS S3 Files mounts through Fargate", aws_mount_tests()),
        TST_CASE("submits Azure whole-container Blob mounts", azure_mount_tests()),
        TST_CASE("runs AWS Batch and recovery", aws_lifecycle_tests()),
        TST_CASE("runs Azure Batch and log recovery", azure_lifecycle_tests()),
        TST_CASE("looks up prices and selects providers", pricing_tests()),
        TST_CASE("formats custom command output", command_output_tests()));
}
