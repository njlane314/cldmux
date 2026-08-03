#include "apps/burst.hpp"

#include <type_traits>

static_assert(std::is_move_constructible_v<burst::prepared_run>);
static_assert(std::is_move_assignable_v<burst::prepared_run>);
static_assert(!std::is_copy_constructible_v<burst::prepared_run>);
static_assert(!std::is_copy_assignable_v<burst::prepared_run>);

int main() {
    burst::request request;
    burst::quote quote;
    burst::receipt receipt;
    burst::core core;
    (void)request;
    (void)quote;
    (void)receipt;
    (void)core;
}
