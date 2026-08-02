#include "cloud.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <iostream>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

template <class F>
void throws(F&& function, std::string_view message) {
  try {
    function();
  } catch (const cloud::error&) {
    return;
  }
  throw std::runtime_error(std::string(message));
}

class fake_server {
 public:
  fake_server() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("socket");
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
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                      &length) != 0)
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
    if (thread_.joinable()) thread_.join();
  }

  [[nodiscard]] std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  std::atomic<int> create_retries{0};
  std::atomic<int> ambiguous_cancel_attempts{0};
  std::atomic<int> cancel_operations{0};
  std::atomic<int> deletes{0};
  std::atomic<int> cursor_queries{0};
  std::atomic<int> overlap_queries{0};
  std::atomic<bool> request_id_changed{false};

 private:
  struct reply {
    int status = 200;
    std::string body = "{}";
  };

  static std::string query(std::string_view target, std::string_view key) {
    const std::string needle = std::string(key) + '=';
    const auto begin = target.find(needle);
    if (begin == std::string_view::npos) return {};
    const auto value = begin + needle.size();
    const auto end = target.find('&', value);
    return std::string(target.substr(value, end - value));
  }

  static std::string job_id(std::string_view target) {
    constexpr std::string_view marker = "/jobs/";
    const auto begin = target.find(marker);
    if (begin == std::string_view::npos) return {};
    const auto value = begin + marker.size();
    const auto end = target.find_first_of("?:", value);
    return std::string(target.substr(value, end - value));
  }

  reply route(std::string_view method, std::string_view target,
              std::string_view body) {
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
      return {200, "{\"name\":\"projects/test-project/locations/europe-west4/jobs/" +
          id + "\",\"uid\":\"uid-" + id +
          "\",\"status\":{\"state\":\"QUEUED\"}}"};
    }

    if (method == "POST" &&
        target.find("/v2/entries:list") != std::string_view::npos) {
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
      if (body.find("2026-01-01T00:00:03.000000000Z") !=
          std::string_view::npos)
        ++overlap_queries;
      if (id.find("cancel-job") != std::string::npos) {
        if (log_attempts_[id]++ == 0) return {200, "{\"entries\":[]}"};
        return {200,
            "{\"entries\":[{\"timestamp\":\"2026-01-01T00:00:05Z\","
            "\"receiveTimestamp\":\"2026-01-01T00:00:05Z\","
            "\"insertId\":\"cancel\",\"textPayload\":\"cancelled\"}]}"};
      }
      if (id.find("retry-success") == std::string::npos)
        return {200, "{\"entries\":[]}"};
      if (log_attempts_[id]++ == 0)
        return {500, "{\"error\":{\"message\":\"logging retry\"}}"};
      if (body.find("\"pageToken\":\"next\"") != std::string_view::npos)
        return {200,
            "{\"entries\":[{\"timestamp\":\"2026-01-01T00:00:02Z\","
            "\"receiveTimestamp\":\"2026-01-01T00:00:02Z\","
            "\"insertId\":\"b\",\"textPayload\":\"second\"},"
            "{\"timestamp\":\"2026-01-01T00:00:03Z\","
            "\"receiveTimestamp\":\"2026-01-01T00:00:03Z\","
            "\"insertId\":\"a\",\"textPayload\":\"third\"},"
            "{\"timestamp\":\"2026-01-01T00:00:04Z\","
            "\"receiveTimestamp\":\"2026-01-01T00:00:04Z\","
            "\"textPayload\":\"fourth\"}]}"};
      return {200,
          "{\"entries\":[{\"timestamp\":\"2026-01-01T00:00:01Z\","
          "\"receiveTimestamp\":\"2026-01-01T00:00:01Z\","
          "\"insertId\":\"a\",\"textPayload\":\"first\"}],"
          "\"nextPageToken\":\"next\"}"};
    }

    if (method == "GET" && target.find("/tasks/0") != std::string_view::npos &&
        target.find("failed-exit-deadline") != std::string_view::npos)
      return {500, "{\"error\":{\"message\":\"task retry\"}}"};

    const std::string id = job_id(target);
    if (method == "POST" && target.ends_with(":cancel")) {
      cancelled_[id] = true;
      if (id.find("ambiguous-cancel") != std::string::npos) {
        ++ambiguous_cancel_attempts;
        return {500, "{\"error\":{\"message\":\"lost cancel response\"}}"};
      }
      return {200, "{\"name\":\"projects/test-project/locations/europe-west4/"
                   "operations/cancel-" + id + "\"}"};
    }
    if (method == "DELETE" && !id.empty()) {
      ++deletes;
      if (id.find("cleanup-failure") != std::string::npos)
        return {403, "{\"error\":{\"message\":\"denied\"}}"};
      return {200, "{\"name\":\"projects/test-project/locations/europe-west4/"
                   "operations/delete-" + id + "\"}"};
    }
    if (method == "GET" &&
        target.find("/operations/") != std::string_view::npos) {
      if (target.find("/cancel-") != std::string_view::npos)
        ++cancel_operations;
      return {200, "{\"done\":true,\"response\":{}}"};
    }
    if (method == "GET" && !id.empty()) {
      std::string state = "SUCCEEDED";
      if (id.find("failed-exit-deadline") != std::string::npos) {
        state = "FAILED";
      } else if (cancelled_[id] &&
                 id.find("ambiguous-cancel") != std::string::npos) {
        const int read = cancellation_reads_[id]++;
        state = read == 0 ? "RUNNING" :
                read == 1 ? "CANCELLATION_IN_PROGRESS" : "CANCELLED";
      } else if (cancelled_[id]) {
        state = "CANCELLED";
      } else if (id.find("cancel") != std::string::npos ||
                 id.find("timeout") != std::string::npos) {
        state = "RUNNING";
      } else if (id.find("retry-success") != std::string::npos &&
                 job_reads_[id]++ == 0) {
        state = "RUNNING";
      }
      return {200, "{\"name\":\"projects/test-project/locations/europe-west4/jobs/" +
          id + "\",\"uid\":\"uid-" + id +
          "\",\"status\":{\"state\":\"" + state + "\"}}"};
    }
    return {404, "{\"error\":{\"message\":\"not found\"}}"};
  }

  static std::string read_request(int socket) {
    std::string request;
    std::array<char, 4096> buffer{};
    std::size_t wanted = 0;
    while (true) {
      const auto count = ::recv(socket, buffer.data(), buffer.size(), 0);
      if (count <= 0) break;
      request.append(buffer.data(), static_cast<std::size_t>(count));
      const auto header_end = request.find("\r\n\r\n");
      if (header_end == std::string::npos) continue;
      if (!wanted) {
        const auto field = request.find("Content-Length:");
        if (field != std::string::npos) {
          const auto begin = request.find_first_not_of(' ', field + 15);
          wanted = static_cast<std::size_t>(std::stoull(request.substr(begin)));
        }
      }
      if (request.size() >= header_end + 4 + wanted) break;
    }
    return request;
  }

  static void write_reply(int socket, const reply& response) {
    const std::string reason = response.status == 200 ? "OK" : "Error";
    const std::string text = "HTTP/1.1 " + std::to_string(response.status) +
        ' ' + reason + "\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(response.body.size()) +
        "\r\nConnection: close\r\n\r\n" + response.body;
    std::size_t sent = 0;
    while (sent < text.size()) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto count = ::send(socket, text.data() + sent, text.size() - sent,
                                flags);
      if (count <= 0) break;
      sent += static_cast<std::size_t>(count);
    }
  }

  void serve() {
    while (!stopping_) {
      const int socket = ::accept(listener_, nullptr, nullptr);
      if (socket < 0) break;
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
      write_reply(socket, route(method, target, body));
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

void lifecycle_tests() {
  fake_server server;
  cloud::config config;
  config.project = "test-project";
  config.region = "europe-west4";
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
  const auto result = success.wait(
      [&](const auto& line) { output.push_back(line.text); });
  check(result.success(), "HTTP success result");
  check(output == std::vector<std::string>(
                      {"first", "second", "third", "fourth"}),
        "composite log identity and fallback deduplication");
  check(success.logs().size() == 4, "cached logs after deletion");
  check(server.create_retries == 1 && !server.request_id_changed,
        "idempotent create retry");
  check(server.cursor_queries > 0, "incremental log cursor");
  check(server.overlap_queries > 0, "log cursor retains an overlap entry");

  spec.name = "cancel-job";
  auto cancelled = client.run(spec);
  cancelled.cancel();
  check(cancelled.wait().state == cloud::job_state::cancelled,
        "observed cancellation");
  check(cancelled.logs().size() == 1 &&
            cancelled.logs().front().text == "cancelled",
        "cancellation drains delayed final logs");
  check(server.cancel_operations > 0, "cancel operation polled");

  spec.name = "ambiguous-cancel";
  auto ambiguous = client.run(spec);
  ambiguous.cancel();
  check(ambiguous.wait().state == cloud::job_state::cancelled &&
            server.ambiguous_cancel_attempts == 4,
        "accepted cancellation survives a lost response");

  spec.name = "timeout-job";
  spec.timeout = std::chrono::milliseconds(1);
  auto timed_out = client.run(spec);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto timeout = timed_out.wait();
  check(timeout.state == cloud::job_state::cancelled &&
            timeout.error().find("timeout") != std::string::npos,
        "controller timeout cancellation");

  spec.name = "cleanup-failure";
  spec.timeout = std::chrono::seconds(5);
  auto cleanup = client.run(spec);
  const auto preserved = cleanup.wait();
  check(preserved.success() && !preserved.warnings.empty(),
        "cleanup failure preserves result");
  check(server.deletes >= 5, "automatic deletion attempted");

  spec.name = "failed-exit-deadline";
  spec.auto_delete = false;
  const auto started = std::chrono::steady_clock::now();
  const auto failed = client.run(spec).wait();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  check(failed.state == cloud::job_state::failed && !failed.exit_code &&
            elapsed < std::chrono::milliseconds(500),
        "failed-task exit lookup respects its deadline");
}

}  // namespace

int main() {
  try {
    cloud::config config;
    config.project = "test-project";
    config.region = "europe";
    config.auth = cloud::auth::bearer("test-token");
    config.estimate_hourly_cost = [](auto, auto, auto machine, bool spot) {
      check(machine == "e2-standard-4", "price machine");
      check(spot, "price spot");
      return std::optional<double>(0.24);
    };
    cloud::client client(std::move(config));

    cloud::job_spec spec{
        .name = "Analysis 42",
        .image = "python:3.13",
        .command = {"python", "analyse.py", "/input/input.csv"},
        .workdir = ".",
        .service_account = "batch-runner@test-project.iam.gserviceaccount.com",
        .mounts = {
            {"cloud://inputs/run-42/", "/input", true},
            {"cloud://results", "/output"},
        },
        .resources = {
            .cpus = 4,
            .memory_gb = 16,
            .gpu = {},
            .spot = true,
            .max_price_per_hour = 0.50,
        },
        .retries = 2,
        .auto_delete = true,
        .timeout = std::chrono::hours(2),
    };

    const auto plan = client.plan(spec);
    check(plan.provider == "gcp", "provider");
    check(plan.region == "europe-west4", "region alias");
    check(plan.machine_type == "e2-standard-4", "machine mapping");
    check(plan.estimated_hourly_cost == 0.24, "price estimate");
    check(client.supports(cloud::feature::spot_instances), "spot support");
    check(!client.supports(cloud::feature::accelerators), "gpu support");

    const std::string body = cloud::detail::batch_body(spec, plan);
    const auto batch = cloud::gcp::detail::parse_json(body);
    const auto* groups = batch.get("taskGroups");
    check(groups && groups->array().size() == 1, "Batch task group");
    check(body.find("\"entrypoint\":\"python\"") != std::string::npos,
          "direct argv entrypoint");
    check(body.find("\"commands\":[\"analyse.py\",\"/input/input.csv\"]") !=
              std::string::npos,
          "direct argv");
    check(body.find("\"cpuMilli\":\"4000\"") != std::string::npos, "cpu");
    check(body.find("\"memoryMib\":\"16384\"") != std::string::npos, "memory");
    check(body.find("\"maxRetryCount\":2") != std::string::npos, "retries");
    check(body.find("\"provisioningModel\":\"SPOT\"") != std::string::npos,
          "spot");
    check(body.find("batch-runner@test-project.iam.gserviceaccount.com") !=
              std::string::npos,
          "service account");
    check(body.find("/mnt/disks/cloud-0:/input:ro") != std::string::npos,
          "read-only mount");
    check(body.find("\"remotePath\":\"inputs/run-42/\"") != std::string::npos,
          "GCS prefix");

    const auto uri = cloud::detail::parse_uri("cloud://bucket/path/to/object");
    check(uri.bucket == "bucket" && uri.key == "path/to/object", "cloud URI");
    check(cloud::gcp::detail::crc32c("123456789") == "4waSgw==", "CRC32C");
    check(cloud::gcp::detail::field(
              cloud::gcp::detail::parse_json("{\"x\":\"\\u03bb\"}"), "x") ==
              "\xce\xbb",
          "JSON unicode");

    const std::string id = cloud::detail::job_id("42 / VERY Long Job Name !!!");
    check(id.size() <= 63 && std::isalpha(static_cast<unsigned char>(id.front())),
          "Batch job id");
    for (const char raw : id) {
      const auto c = static_cast<unsigned char>(raw);
      check(std::islower(c) || std::isdigit(c) || c == '-', "Batch job id chars");
    }

    auto too_expensive = spec;
    too_expensive.resources.max_price_per_hour = 0.10;
    throws([&] { (void)client.plan(too_expensive); }, "price ceiling");

    auto gpu = spec;
    gpu.resources.gpu = "L4";
    throws([&] { (void)client.plan(gpu); }, "unsupported GPU");

    auto invalid_price = spec;
    invalid_price.resources.max_price_per_hour =
        std::numeric_limits<double>::quiet_NaN();
    throws([&] { (void)client.plan(invalid_price); }, "invalid price ceiling");

    auto invalid_job = spec;
    invalid_job.command.clear();
    throws([&] { (void)client.plan(invalid_job); }, "plan validates command");
    invalid_job = spec;
    invalid_job.retries = 11;
    throws([&] { (void)client.plan(invalid_job); }, "plan validates retries");
    invalid_job = spec;
    invalid_job.mounts.front().source = "cloud://inputs/one-object";
    throws([&] { (void)client.plan(invalid_job); }, "prefix-only mounts");

    cloud::config unsupported;
    unsupported.provider = "aws";
    unsupported.project = "test";
    unsupported.auth = cloud::auth::bearer("test");
    cloud::client aws(std::move(unsupported));
    throws([&] { (void)aws.plan(spec); }, "unsupported AWS");
    throws([&] { (void)aws.storage().list("cloud://bucket"); },
           "unsupported AWS storage");

    cloud::config automatic;
    automatic.project = "test";
    automatic.selection = cloud::selection::lowest_cost;
    automatic.auth = cloud::auth::bearer("test");
    cloud::client cheapest(std::move(automatic));
    throws([&] { (void)cheapest.plan(spec); }, "unsupported lowest cost");

    cloud::config unsafe;
    unsafe.project = "test";
    unsafe.region = "europe-west4\"}";
    unsafe.auth = cloud::auth::bearer("test");
    cloud::client unsafe_region(std::move(unsafe));
    throws([&] { (void)unsafe_region.plan(spec); }, "region validation");

    lifecycle_tests();

    std::cout << "ok\n";
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
