#include <cloud/detail/config.hpp>

int main() {
    cloud::gcp::Config config;
    return config.timeout.count() > 0 ? 0 : 1;
}
