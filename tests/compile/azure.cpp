#include <cloud/detail/providers/azure.hpp>

int main() {
    const auto function = &cloud::detail::azure_compute_create;
    return function ? 0 : 1;
}
