#include <cldmux/detail/http.hpp>

int main() {
    cldmux::gcp::detail::HttpRequest request;
    return request.method == "GET" ? 0 : 1;
}
