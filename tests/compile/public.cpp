#include <cldmux>

int main() {
    cldmux::job_spec job;
    job.name = "compile-probe";
    cldmux::command_output output;
    output.add("job_name", job.name);

    cldmux::config config;
    config.auth = cldmux::auth::from(cldmux::token_provider([] {
        return cldmux::access_token{
            "compile-token", std::chrono::system_clock::time_point::max(), {}};
    }));
    config.lookup_hourly_cost = [](const cldmux::price_request&) {
        return std::optional<double>{1.0};
    };
    config.aws.credentials = [] {
        return cldmux::aws_credentials{"key", "secret", "session"};
    };
    cldmux::router router(std::move(config));
    (void)router;
    return output.records().size() == 1 ? 0 : 1;
}
