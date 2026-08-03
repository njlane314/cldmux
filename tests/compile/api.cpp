#include <cldmux/api.hpp>

int main() {
    cldmux::job_spec job;
    return job.resources.cpus == 1 ? 0 : 1;
}
