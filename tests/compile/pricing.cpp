#include <cloud/detail/pricing.hpp>

int main() {
    const auto function = &cloud::detail::catalogue_price;
    return function ? 0 : 1;
}
