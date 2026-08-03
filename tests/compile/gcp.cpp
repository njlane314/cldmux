#include <cloud/detail/providers/gcp.hpp>

int main() {
    cloud::gcp::Config config;
    return config.batch_endpoint.empty() ? 1 : 0;
}
