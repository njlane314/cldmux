#include <cloud/job.hpp>

int main() {
    cloud::result result;
    return result.state == cloud::job_state::unknown ? 0 : 1;
}
