#include <cldmux/detail/provider.hpp>

int main() {
    return cldmux::detail::retryable(503) ? 0 : 1;
}
