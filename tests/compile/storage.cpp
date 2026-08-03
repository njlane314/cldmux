#include <cloud/detail/storage.hpp>

int main() {
    const auto function = &cloud::detail::aws_storage_stat;
    return function ? 0 : 1;
}
