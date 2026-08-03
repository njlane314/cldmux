#include <cloud>

int cloud_odr_a() {
    cloud::job_spec job;
    return job.resources.cpus == 1 ? CLOUD_H_VERSION_NUM : 0;
}
