#include <cloud/plan.hpp>

int main() {
    cloud::plan plan;
    return plan.provider == "gcp" ? 0 : 1;
}
