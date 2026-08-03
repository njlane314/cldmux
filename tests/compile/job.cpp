#include <cldmux/job.hpp>

int main() {
    cldmux::result result;
    return result.state == cldmux::job_state::unknown ? 0 : 1;
}
