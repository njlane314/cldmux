#include <cloud/detail/submission.hpp>

int main() {
    const auto function = &cloud::detail::submit_aws;
    return function ? 0 : 1;
}
