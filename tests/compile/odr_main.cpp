int cloud_odr_a();
int cloud_odr_b();

int main() {
    return cloud_odr_a() == cloud_odr_b() && cloud_odr_a() != 0 ? 0 : 1;
}
