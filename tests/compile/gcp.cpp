#include <cldmux/detail/providers/gcp.hpp>

int main() {
    cldmux::gcp::Config config;
    return config.batch_endpoint.empty() ? 1 : 0;
}
