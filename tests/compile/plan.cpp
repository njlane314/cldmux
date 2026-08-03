#include <cldmux/plan.hpp>

int main() {
    cldmux::plan plan;
    return plan.provider == "gcp" ? 0 : 1;
}
