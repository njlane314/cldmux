#include <cldmux>

int cldmux_odr_b() {
    cldmux::command_output output;
    output.add("probe", "b");
    return output.records().size() == 1 ? CLDMUX_VERSION_NUM : 0;
}
