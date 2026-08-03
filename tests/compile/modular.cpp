#include <cloud/cloud.hpp>

int main() {
    cloud::job_spec job;
    job.name = "compile-probe";
    cloud::command_output output;
    output.add("job_name", job.name);

    cloud::config config;
    config.auth = cloud::auth::from(cloud::token_provider([] {
        return cloud::access_token{
            "compile-token", std::chrono::system_clock::time_point::max(), {}};
    }));
    config.lookup_hourly_cost = [](const cloud::price_request&) {
        return std::optional<double>{1.0};
    };
    config.aws.credentials = [] {
        return cloud::aws_credentials{"key", "secret", "session"};
    };
    cloud::client client(std::move(config));
    (void)client;
    return output.records().size() == 1 ? 0 : 1;
}
