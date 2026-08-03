#!/bin/sh

set -eu

tool_input=$1
tool_directory=$(CDPATH= cd -- "$(dirname -- "$tool_input")" && pwd)
tool=$tool_directory/$(basename -- "$tool_input")
compiler=${CXX:-c++}
compiler_flags=${CXXFLAGS:-}
sandbox=$(mktemp -d "${TMPDIR:-/tmp}/cldmux-amalgamate.XXXXXX")
trap 'rm -rf "$sandbox"' EXIT HUP INT TERM

fail() {
    printf '%s\n' "amalgamate test: $1" >&2
    exit 1
}

mkdir -p "$sandbox/include/cldmux"
printf '%s\n' \
    '#pragma once' \
    '#include "cldmux/a.hpp"' \
    '#include "cldmux/b.hpp"' \
    '#include <vector>' \
    '#include "third_party.hpp"' \
    'inline int root_value() { return a_value() + b_value(); }' \
    > "$sandbox/include/cldmux/root.hpp"
printf '%s\n' \
    '#pragma once' \
    '#include "cldmux/shared.hpp"' \
    'inline int a_value() { return shared_value(); }' \
    > "$sandbox/include/cldmux/a.hpp"
printf '%s\n' \
    '#ifndef CLDMUX_B_HPP' \
    '#define CLDMUX_B_HPP' \
    '#include "cldmux/shared.hpp"' \
    'inline int b_value() { return shared_value(); }' \
    '#endif // CLDMUX_B_HPP' \
    > "$sandbox/include/cldmux/b.hpp"
printf '#pragma once\r\ninline int shared_value() { return 1; }\r\n' \
    > "$sandbox/include/cldmux/shared.hpp"
printf '%s\n' '#pragma once' > "$sandbox/third_party.hpp"

root=$sandbox/include/cldmux/root.hpp
include_root=$sandbox/include
output=$sandbox/cldmux

"$tool" --root "$root" --include-root "$include_root" --output "$output" --write
"$tool" --root "$root" --include-root "$include_root" --output "$output" --check
"$tool" --root "$root" --include-root "$include_root" --output "$output" --stdout \
    | cmp -s - "$output"

printf '%s\n' 'inline int newly_added_value() { return 3; }' \
    >> "$sandbox/include/cldmux/shared.hpp"
if "$tool" --root "$root" --include-root "$include_root" --output "$output" --check \
    >/dev/null 2>&1; then
    fail "stale generated output was accepted"
fi
printf '#pragma once\r\ninline int shared_value() { return 1; }\r\n' \
    > "$sandbox/include/cldmux/shared.hpp"
"$tool" --root "$root" --include-root "$include_root" --output "$output" --check

modules=$sandbox/modules.txt
"$tool" --root "$root" --include-root "$include_root" --list-modules > "$modules"
test "$(wc -l < "$modules" | tr -d ' ')" -eq 4 || fail "unexpected module count"
test "$(sort -u "$modules" | wc -l | tr -d ' ')" -eq 4 || fail "module repeated"
test "$(grep -c '^cldmux/shared.hpp$' "$modules")" -eq 1 || fail "diamond repeated"

grep -q '^#include <vector>$' "$output" || fail "system include was removed"
grep -q '^#include "third_party.hpp"$' "$output" || fail "external include was removed"
! grep -q 'include "cldmux/' "$output" || fail "project include was retained"
! grep -q '#pragma once' "$output" || fail "pragma once was retained"
! grep -q 'CLDMUX_B_HPP' "$output" || fail "module guard was retained"
! LC_ALL=C grep "$(printf '\r')" "$output" >/dev/null || fail "CRLF was retained"

printf '%s\n' \
    '#ifndef CLDMUX_ALTERNATE_GUARD_HPP' \
    '#define CLDMUX_ALTERNATE_GUARD_HPP' \
    'inline int guarded_value() { return 1; }' \
    '#else' \
    'inline int guarded_value() { return 2; }' \
    '#endif' \
    > "$sandbox/include/cldmux/alternate_guard.hpp"
alternate=$sandbox/alternate.h
"$tool" --root "$sandbox/include/cldmux/alternate_guard.hpp" \
    --include-root "$include_root" --output "$alternate" --write
grep -q '^#ifndef CLDMUX_ALTERNATE_GUARD_HPP$' "$alternate" || \
    fail "alternate module guard was stripped"
grep -q '^#else$' "$alternate" || fail "alternate guard branch was removed"

printf '%s\n' '#include <cldmux>' 'int main() { return root_value() == 2 ? 0 : 1; }' \
    > "$sandbox/consumer.cpp"
# compiler_flags is intentionally word-split so callers can pass ordinary flags.
# shellcheck disable=SC2086
"$compiler" $compiler_flags -std=c++17 -I"$sandbox" "$sandbox/consumer.cpp" \
    -o "$sandbox/consumer"
"$sandbox/consumer"

printf '%s\n' '#include "cldmux/cycle_b.hpp"' > "$sandbox/include/cldmux/cycle_a.hpp"
printf '%s\n' '#include "cldmux/cycle_a.hpp"' > "$sandbox/include/cldmux/cycle_b.hpp"
if "$tool" --root "$sandbox/include/cldmux/cycle_a.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "cycle was accepted"
fi

printf '%s\n' '#include "cldmux/missing.hpp"' > "$sandbox/include/cldmux/missing_root.hpp"
if "$tool" --root "$sandbox/include/cldmux/missing_root.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "missing module was accepted"
fi

printf '%s\n' '#pragma once' > "$sandbox/outside.hpp"
ln -s ../../outside.hpp "$sandbox/include/cldmux/outside.hpp"
printf '%s\n' '#include "cldmux/outside.hpp"' > "$sandbox/include/cldmux/outside_root.hpp"
if "$tool" --root "$sandbox/include/cldmux/outside_root.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "module outside include root was accepted"
fi

printf '%s\n' '#include_next "cldmux/a.hpp"' > "$sandbox/include/cldmux/ambiguous.hpp"
if "$tool" --root "$sandbox/include/cldmux/ambiguous.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "ambiguous include was accepted"
fi

printf '%s\n' '#include CLDMUX_HEADER' > "$sandbox/include/cldmux/macro_include.hpp"
if "$tool" --root "$sandbox/include/cldmux/macro_include.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "macro include was accepted"
fi

printf '%s\n' '/* leading comment */ #include "cldmux/a.hpp"' \
    > "$sandbox/include/cldmux/commented_include.hpp"
if "$tool" --root "$sandbox/include/cldmux/commented_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "comment-obscured project include was accepted"
fi

printf '%s\n' '/* leading comment' '*/ #include "cldmux/a.hpp"' \
    > "$sandbox/include/cldmux/multiline_comment_include.hpp"
if "$tool" --root "$sandbox/include/cldmux/multiline_comment_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "multiline-comment project include was accepted"
fi

printf '%s\n' '#/**/include "cldmux/a.hpp"' \
    > "$sandbox/include/cldmux/commented_directive.hpp"
if "$tool" --root "$sandbox/include/cldmux/commented_directive.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "commented include directive was accepted"
fi

printf '%s\n' '#/**/include CLDMUX_HEADER' \
    > "$sandbox/include/cldmux/commented_macro_include.hpp"
if "$tool" --root "$sandbox/include/cldmux/commented_macro_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "commented macro include directive was accepted"
fi

printf '%s\n' '%:include CLDMUX_HEADER' > "$sandbox/include/cldmux/digraph_include.hpp"
if "$tool" --root "$sandbox/include/cldmux/digraph_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "digraph include directive was accepted"
fi

printf '%s\n' '#inc\' 'lude CLDMUX_HEADER' > "$sandbox/include/cldmux/spliced_include.hpp"
if "$tool" --root "$sandbox/include/cldmux/spliced_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "spliced include directive was accepted"
fi

printf '%s\n' 'inline const char* harmless() { return "cldmux/a.hpp"; } // #include' \
    > "$sandbox/include/cldmux/harmless_literal.hpp"
"$tool" --root "$sandbox/include/cldmux/harmless_literal.hpp" \
    --include-root "$include_root" --stdout >/dev/null

printf '%s\n' '#include <vector>' '#include "cldmux/a.hpp"' \
    > "$sandbox/include/cldmux/late_include.hpp"
if "$tool" --root "$sandbox/include/cldmux/late_include.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "late project include was accepted"
fi

if "$tool" --root "$root" --include-root "$include_root" --write --check \
    >/dev/null 2>&1; then
    fail "conflicting actions were accepted"
fi
