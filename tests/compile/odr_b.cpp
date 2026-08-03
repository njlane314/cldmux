#include <cloud>

int cloud_odr_b() {
    cloud::command_output output;
    output.add("probe", "b");
    return output.records().size() == 1 ? CLOUD_H_VERSION_NUM : 0;
}
