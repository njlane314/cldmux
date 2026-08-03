#include "apps/dispatch.hpp"

#include <type_traits>

static_assert(std::is_move_constructible_v<dispatch::prepared_run>);
static_assert(std::is_move_assignable_v<dispatch::prepared_run>);
static_assert(!std::is_copy_constructible_v<dispatch::prepared_run>);
static_assert(!std::is_copy_assignable_v<dispatch::prepared_run>);

int main() {
    dispatch::request request;
    dispatch::quote quote;
    dispatch::receipt receipt;
    dispatch::core core;
    (void)request;
    (void)quote;
    (void)receipt;
    (void)core;
}
