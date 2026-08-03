#include <cldmux>

int cldmux_odr_a() {
    cldmux::job_spec job;
    return job.resources.cpus == 1 ? CLDMUX_VERSION_NUM : 0;
}
