#include <cloud/detail/provider.hpp>

int main() {
    return cloud::detail::retryable(503) ? 0 : 1;
}
