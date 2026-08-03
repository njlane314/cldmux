#include <cloud/detail/json.hpp>

int main() {
    const auto value = cloud::gcp::detail::parse_json("null");
    return value.text("null") == "null" ? 0 : 1;
}
