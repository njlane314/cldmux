#include <cloud/detail/http.hpp>

int main() {
    cloud::gcp::detail::HttpRequest request;
    return request.method == "GET" ? 0 : 1;
}
