#include <cloud/api.hpp>

int main() {
    cloud::job_spec job;
    return job.resources.cpus == 1 ? 0 : 1;
}
