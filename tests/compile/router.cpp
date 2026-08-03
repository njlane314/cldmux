#include <cldmux/router.hpp>

#include <type_traits>
#include <utility>

int main() {
    using routed = decltype(std::declval<const cldmux::router&>().route(
        std::declval<const cldmux::job_spec&>()));
    static_assert(std::is_same_v<routed, cldmux::client>);
    static_assert(!std::is_default_constructible_v<cldmux::client>);
    static_assert(!std::is_constructible_v<cldmux::client, cldmux::config>);
    return 0;
}
