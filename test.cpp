#include "cloud.h"

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
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {

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
    std::atomic<int> azure_price_reads{0};
    std::atomic<int> aws_spot_reads{0};
    std::atomic<bool> aws_body_valid{true};
    std::atomic<bool> aws_ambiguous_register{false};
    std::atomic<bool> aws_endless_logs{false};
    std::atomic<bool> azure_body_valid{true};
    std::atomic<bool> corrupt_download{false};
    std::atomic<bool> request_id_changed{false};

private:
    struct reply {
        int status = 200;
        std::string body = "{}";
    };

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

    reply route(std::string_view method, std::string_view target, std::string_view body) {
        if (method == "POST" && target == "/v1/registerjobdefinition") {
            ++aws_registers;
            try {
                (void)cloud::gcp::detail::parse_json(body);
            } catch (const cloud::error&) {
                aws_body_valid = false;
            }
            if (body.find("\"platformCapabilities\":[\"EC2\"]") == std::string_view::npos ||
                body.find("\"type\":\"VCPU\",\"value\":\"4\"") == std::string_view::npos ||
                body.find("\"type\":\"GPU\",\"value\":\"1\"") == std::string_view::npos)
                aws_body_valid = false;
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
            try {
                (void)cloud::gcp::detail::parse_json(body);
            } catch (const cloud::error&) {
                aws_body_valid = false;
            }
            if (body.find("\"jobQueue\":\"gpu-queue\"") == std::string_view::npos ||
                body.find("\"attempts\":2") == std::string_view::npos ||
                body.find("\"tags\":{\"cloud-hpp\":\"temporary\"}") == std::string_view::npos)
                aws_body_valid = false;
            return {200, "{\"jobId\":\"aws-job-id\",\"jobName\":\"cloud-job\"}"};
        }
        if (method == "POST" && target == "/v1/describejobs" && aws_cancel_mode != 0) {
            if (aws_cancel_accepted)
                return {200,
                        "{\"jobs\":[{\"jobId\":\"aws-job-id\",\"status\":\"FAILED\","
                        "\"statusReason\":\"cloud.h cancellation\","
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
            return {200, "{\"skus\":[" + gcp_sku("E2 Instance Core", "OnDemand", 10'000'000) + ',' +
                             gcp_sku("E2 Custom Instance Core", "OnDemand", 11'000'000) + ',' +
                             gcp_sku("E2 Instance Ram", "OnDemand", 2'000'000) + ',' +
                             gcp_sku("E2 Custom Instance Ram", "OnDemand", 3'000'000) + ',' +
                             gcp_sku("E2 Instance Core", "Commit1Yr", 4'000'000) + ',' +
                             gcp_sku("G2 Instance Core", "OnDemand", 20'000'000) + ',' +
                             gcp_sku("G2 Custom Instance Core", "OnDemand", 21'000'000) + ',' +
                             gcp_sku("G2 Sole Tenancy Instance Core", "OnDemand", 22'000'000) +
                             ',' + gcp_sku("G2 Instance Ram", "OnDemand", 3'000'000) + ',' +
                             gcp_sku("G2 Custom Instance Ram", "OnDemand", 4'000'000) + ',' +
                             gcp_sku("G2 Sole Tenancy Instance Ram", "OnDemand", 5'000'000) + ',' +
                             gcp_sku("Nvidia L4 GPU", "OnDemand", 500'000'000) + ',' +
                             gcp_sku("Nvidia L4 GPU", "Commit1Yr", 400'000'000) + "]}"};
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
                "\"retailPrice\":0.05}],\"NextPageLink\":null}"};
        }

        if (method == "POST" && starts_with(target, "/jobs?api-version=")) {
            const int writes = ++azure_jobs;
            try {
                (void)cloud::gcp::detail::parse_json(body);
            } catch (const cloud::error&) {
                azure_body_valid = false;
            }
            if (body.find("\"vmSize\":\"Standard_NC24ads_A100_v4\"") == std::string_view::npos ||
                body.find("\"targetLowPriorityNodes\":1") == std::string_view::npos)
                azure_body_valid = false;
            if (writes == 1)
                return {500, "{\"code\":\"ambiguous\"}"};
            return {};
        }
        if (method == "POST" && target.find("/tasks?api-version=") != std::string_view::npos) {
            const int writes = ++azure_tasks;
            try {
                (void)cloud::gcp::detail::parse_json(body);
            } catch (const cloud::error&) {
                azure_body_valid = false;
            }
            if (body.find("\"commandLine\":\"run --fast\"") == std::string_view::npos ||
                body.find("\"imageName\":\"image\"") == std::string_view::npos)
                azure_body_valid = false;
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
            return {200, "{\"done\":true,\"response\":{}}"};
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
        const std::string text = "HTTP/1.1 " + std::to_string(response.status) + ' ' + reason +
                                 "\r\nContent-Type: application/json\r\nContent-Length: " +
                                 std::to_string(response.body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + response.body;
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
            const std::string_view body = header_end == std::string::npos
                                              ? std::string_view{}
                                              : std::string_view(request).substr(header_end + 4);
            const auto response = route(method, target, body);
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
};

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
                   !aws.supports("aws", cloud::feature::object_storage),
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
    gcp_prices.prices = cloud::price_source::public_catalog;
    cloud::client gcp_priced(std::move(gcp_prices));
    tst::check(std::fabs(*gcp_priced.plan(price_spec).estimated_hourly_cost - 0.072) < 1e-12,
               "GCP built-in component price");
    auto exact_catalog_ceiling = price_spec;
    exact_catalog_ceiling.resources.max_price_per_hour = 0.072;
    (void)gcp_priced.plan(exact_catalog_ceiling);
    auto gcp_l4_price = price_spec;
    gcp_l4_price.resources.gpu = "l4";
    tst::check(std::fabs(*gcp_priced.plan(gcp_l4_price).estimated_hourly_cost - 0.628) < 1e-12,
               "GCP GPU catalogue price ignores custom, sole-tenancy, and commitment SKUs");
    (void)gcp_priced.plan(gcp_l4_price);
    tst::check(server.gcp_price_reads == 2, "GCP price cache");

    auto gcp_bundled_disk_price = price_spec;
    gcp_bundled_disk_price.resources.gpu = "a100";
    tst::check(!gcp_priced.plan(gcp_bundled_disk_price).estimated_hourly_cost,
               "GCP bundled Local SSD shape has no incomplete catalogue quote");
    gcp_bundled_disk_price.resources.max_price_per_hour = 100;
    tst::throws<cloud::error>([&] { (void)gcp_priced.plan(gcp_bundled_disk_price); },
                              "GCP bundled Local SSD price ceiling fails closed");
    tst::check(server.gcp_price_reads == 2,
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
                   observed_price_request->accelerator_count == 2 && observed_price_request->spot,
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

    cloud::config aws_prices;
    aws_prices.provider = "aws";
    aws_prices.region = "eu-west-1";
    aws_prices.allow_insecure_http = true;
    aws_prices.aws.access_key_id = "AKIDEXAMPLE";
    aws_prices.aws.secret_access_key = "secret";
    aws_prices.aws.job_queue = "cpu-queue";
    aws_prices.aws.machine_type = "m6i.xlarge";
    aws_prices.aws.pricing_endpoint = server.url();
    aws_prices.prices = cloud::price_source::public_catalog;
    cloud::client aws_priced(std::move(aws_prices));
    price_spec.timeout = std::chrono::seconds(60);
    tst::check(aws_priced.plan(price_spec).estimated_hourly_cost == 0.42,
               "AWS built-in on-demand price");

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
    aws_spot_prices.prices = cloud::price_source::public_catalog;
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
    azure_prices.prices = cloud::price_source::public_catalog;
    cloud::client azure_priced(std::move(azure_prices));
    tst::check(azure_priced.plan(price_spec).estimated_hourly_cost == 0.20,
               "Azure built-in retail price");
    price_spec.resources.spot = true;
    tst::check(azure_priced.plan(price_spec).estimated_hourly_cost == 0.04,
               "Azure built-in Spot price");
    price_spec.resources.spot = false;

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
    tst::check(cheapest.plan(price_spec).provider == "azure", "lowest-cost provider selection");
}

void environment_factory_tests() {
    environment_guard environment{
        "CLOUD_REGION",
        "CLOUD_ZONE",
        "CLOUD_GCP_PROJECT",
        "CLOUD_GCP_REGION",
        "CLOUD_GCP_ZONE",
        "CLOUD_AWS_JOB_QUEUE",
        "CLOUD_AWS_SPOT_JOB_QUEUE",
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
    };

    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("unknown"); },
                              "environment factory rejects an unknown provider");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("gcp"); },
                              "environment factory requires a GCP project");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("aws"); },
                              "environment factory requires an AWS queue");
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

    environment.set("CLOUD_REGION", "us");
    environment.set("CLOUD_ZONE", "shared-zone");
    environment.set("CLOUD_GCP_PROJECT", "test-project");
    tst::throws<cloud::error>([] { (void)cloud::client::from_environment("cheapest"); },
                              "cheapest routing requires at least two providers");
    environment.set("CLOUD_GCP_REGION", "europe-west4");
    environment.set("CLOUD_GCP_ZONE", "europe-west4-a");

    const cloud::config gcp_config = cloud::detail::config_from_environment("gcp");
    tst::check(gcp_config.provider && *gcp_config.provider == "gcp" &&
                   gcp_config.project == "test-project" && gcp_config.region == "us" &&
                   gcp_config.zone == "shared-zone" &&
                   cloud::detail::configured_region(gcp_config, "gcp") == "europe-west4" &&
                   cloud::detail::configured_zone(gcp_config, "gcp") == "europe-west4-a",
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

    const cloud::config aws_config = cloud::detail::config_from_environment("aws");
    tst::check(aws_config.provider && *aws_config.provider == "aws" &&
                   aws_config.aws.job_queue == "cpu-queue" &&
                   aws_config.aws.spot_job_queue == "cpu-spot-queue" &&
                   aws_config.aws.machine_type == "m6i.xlarge" &&
                   aws_config.aws.spot_machine_type == "m6i.xlarge" &&
                   aws_config.aws.log_group == "/aws/batch/cloud-test" &&
                   cloud::detail::configured_region(aws_config, "aws") == "eu-west-1" &&
                   cloud::detail::configured_zone(aws_config, "aws") == "eu-west-1a",
               "AWS environment mapping");
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

    const cloud::config azure_config = cloud::detail::config_from_environment("azure");
    tst::check(azure_config.provider && *azure_config.provider == "azure" &&
                   azure_config.azure.batch_endpoint == "https://test.westeurope.batch.azure.com" &&
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
                   cheapest_config.prices == cloud::price_source::public_catalog,
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
    static_assert(CLOUD_H_VERSION_NUM == 0x000200);
    static_assert(std::is_aggregate_v<cloud::resources>);
    static_assert(std::is_aggregate_v<cloud::job_spec>);

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

} // namespace

int main() {
    return tst::run(
        TST_CASE("plans and validates GCP workloads", planning_tests()),
        TST_CASE("constructs clients from the environment", environment_factory_tests()),
        TST_CASE("runs GCP lifecycle, storage, and compute", gcp_lifecycle_tests()),
        TST_CASE("runs AWS Batch and recovery", aws_lifecycle_tests()),
        TST_CASE("runs Azure Batch and log recovery", azure_lifecycle_tests()),
        TST_CASE("looks up prices and selects providers", pricing_tests()));
}
