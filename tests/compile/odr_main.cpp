int cldmux_odr_a();
int cldmux_odr_b();

int main() {
    return cldmux_odr_a() == cldmux_odr_b() && cldmux_odr_a() != 0 ? 0 : 1;
}
