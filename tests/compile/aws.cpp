#include <cloud/detail/providers/aws.hpp>

int main() {
    const auto function = &cloud::detail::aws_compute_create;
    return function ? 0 : 1;
}
