#include <cldmux/detail/config.hpp>

int main() {
    cldmux::gcp::Config config;
    return config.timeout.count() > 0 ? 0 : 1;
}
