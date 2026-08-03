#!/bin/sh

set -eu

tool_input=$1
tool_directory=$(CDPATH= cd -- "$(dirname -- "$tool_input")" && pwd)
tool=$tool_directory/$(basename -- "$tool_input")
compiler=${CXX:-c++}
compiler_flags=${CXXFLAGS:-}
sandbox=$(mktemp -d "${TMPDIR:-/tmp}/cloud-amalgamate.XXXXXX")
trap 'rm -rf "$sandbox"' EXIT HUP INT TERM

fail() {
    printf '%s\n' "amalgamate test: $1" >&2
    exit 1
}

mkdir -p "$sandbox/include/cloud"
printf '%s\n' \
    '#pragma once' \
    '#include "cloud/a.hpp"' \
    '#include "cloud/b.hpp"' \
    '#include <vector>' \
    '#include "third_party.hpp"' \
    'inline int root_value() { return a_value() + b_value(); }' \
    > "$sandbox/include/cloud/root.hpp"
printf '%s\n' \
    '#pragma once' \
    '#include "cloud/shared.hpp"' \
    'inline int a_value() { return shared_value(); }' \
    > "$sandbox/include/cloud/a.hpp"
printf '%s\n' \
    '#ifndef CLOUD_B_HPP' \
    '#define CLOUD_B_HPP' \
    '#include "cloud/shared.hpp"' \
    'inline int b_value() { return shared_value(); }' \
    '#endif // CLOUD_B_HPP' \
    > "$sandbox/include/cloud/b.hpp"
printf '#pragma once\r\ninline int shared_value() { return 1; }\r\n' \
    > "$sandbox/include/cloud/shared.hpp"
printf '%s\n' '#pragma once' > "$sandbox/third_party.hpp"

root=$sandbox/include/cloud/root.hpp
include_root=$sandbox/include
output=$sandbox/cloud

"$tool" --root "$root" --include-root "$include_root" --output "$output" --write
"$tool" --root "$root" --include-root "$include_root" --output "$output" --check
"$tool" --root "$root" --include-root "$include_root" --output "$output" --stdout \
    | cmp -s - "$output"

printf '%s\n' 'inline int newly_added_value() { return 3; }' \
    >> "$sandbox/include/cloud/shared.hpp"
if "$tool" --root "$root" --include-root "$include_root" --output "$output" --check \
    >/dev/null 2>&1; then
    fail "stale generated output was accepted"
fi
printf '#pragma once\r\ninline int shared_value() { return 1; }\r\n' \
    > "$sandbox/include/cloud/shared.hpp"
"$tool" --root "$root" --include-root "$include_root" --output "$output" --check

modules=$sandbox/modules.txt
"$tool" --root "$root" --include-root "$include_root" --list-modules > "$modules"
test "$(wc -l < "$modules" | tr -d ' ')" -eq 4 || fail "unexpected module count"
test "$(sort -u "$modules" | wc -l | tr -d ' ')" -eq 4 || fail "module repeated"
test "$(grep -c '^cloud/shared.hpp$' "$modules")" -eq 1 || fail "diamond repeated"

grep -q '^#include <vector>$' "$output" || fail "system include was removed"
grep -q '^#include "third_party.hpp"$' "$output" || fail "external include was removed"
! grep -q 'include "cloud/' "$output" || fail "project include was retained"
! grep -q '#pragma once' "$output" || fail "pragma once was retained"
! grep -q 'CLOUD_B_HPP' "$output" || fail "module guard was retained"
! LC_ALL=C grep "$(printf '\r')" "$output" >/dev/null || fail "CRLF was retained"

printf '%s\n' \
    '#ifndef CLOUD_ALTERNATE_GUARD_HPP' \
    '#define CLOUD_ALTERNATE_GUARD_HPP' \
    'inline int guarded_value() { return 1; }' \
    '#else' \
    'inline int guarded_value() { return 2; }' \
    '#endif' \
    > "$sandbox/include/cloud/alternate_guard.hpp"
alternate=$sandbox/alternate.h
"$tool" --root "$sandbox/include/cloud/alternate_guard.hpp" \
    --include-root "$include_root" --output "$alternate" --write
grep -q '^#ifndef CLOUD_ALTERNATE_GUARD_HPP$' "$alternate" || \
    fail "alternate module guard was stripped"
grep -q '^#else$' "$alternate" || fail "alternate guard branch was removed"

printf '%s\n' '#include <cloud>' 'int main() { return root_value() == 2 ? 0 : 1; }' \
    > "$sandbox/consumer.cpp"
# compiler_flags is intentionally word-split so callers can pass ordinary flags.
# shellcheck disable=SC2086
"$compiler" $compiler_flags -std=c++17 -I"$sandbox" "$sandbox/consumer.cpp" \
    -o "$sandbox/consumer"
"$sandbox/consumer"

printf '%s\n' '#include "cloud/cycle_b.hpp"' > "$sandbox/include/cloud/cycle_a.hpp"
printf '%s\n' '#include "cloud/cycle_a.hpp"' > "$sandbox/include/cloud/cycle_b.hpp"
if "$tool" --root "$sandbox/include/cloud/cycle_a.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "cycle was accepted"
fi

printf '%s\n' '#include "cloud/missing.hpp"' > "$sandbox/include/cloud/missing_root.hpp"
if "$tool" --root "$sandbox/include/cloud/missing_root.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "missing module was accepted"
fi

printf '%s\n' '#pragma once' > "$sandbox/outside.hpp"
ln -s ../../outside.hpp "$sandbox/include/cloud/outside.hpp"
printf '%s\n' '#include "cloud/outside.hpp"' > "$sandbox/include/cloud/outside_root.hpp"
if "$tool" --root "$sandbox/include/cloud/outside_root.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "module outside include root was accepted"
fi

printf '%s\n' '#include_next "cloud/a.hpp"' > "$sandbox/include/cloud/ambiguous.hpp"
if "$tool" --root "$sandbox/include/cloud/ambiguous.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "ambiguous include was accepted"
fi

printf '%s\n' '#include CLOUD_HEADER' > "$sandbox/include/cloud/macro_include.hpp"
if "$tool" --root "$sandbox/include/cloud/macro_include.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "macro include was accepted"
fi

printf '%s\n' '/* leading comment */ #include "cloud/a.hpp"' \
    > "$sandbox/include/cloud/commented_include.hpp"
if "$tool" --root "$sandbox/include/cloud/commented_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "comment-obscured project include was accepted"
fi

printf '%s\n' '/* leading comment' '*/ #include "cloud/a.hpp"' \
    > "$sandbox/include/cloud/multiline_comment_include.hpp"
if "$tool" --root "$sandbox/include/cloud/multiline_comment_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "multiline-comment project include was accepted"
fi

printf '%s\n' '#/**/include "cloud/a.hpp"' \
    > "$sandbox/include/cloud/commented_directive.hpp"
if "$tool" --root "$sandbox/include/cloud/commented_directive.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "commented include directive was accepted"
fi

printf '%s\n' '#/**/include CLOUD_HEADER' \
    > "$sandbox/include/cloud/commented_macro_include.hpp"
if "$tool" --root "$sandbox/include/cloud/commented_macro_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "commented macro include directive was accepted"
fi

printf '%s\n' '%:include CLOUD_HEADER' > "$sandbox/include/cloud/digraph_include.hpp"
if "$tool" --root "$sandbox/include/cloud/digraph_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "digraph include directive was accepted"
fi

printf '%s\n' '#inc\' 'lude CLOUD_HEADER' > "$sandbox/include/cloud/spliced_include.hpp"
if "$tool" --root "$sandbox/include/cloud/spliced_include.hpp" \
    --include-root "$include_root" --stdout >/dev/null 2>&1; then
    fail "spliced include directive was accepted"
fi

printf '%s\n' 'inline const char* harmless() { return "cloud/a.hpp"; } // #include' \
    > "$sandbox/include/cloud/harmless_literal.hpp"
"$tool" --root "$sandbox/include/cloud/harmless_literal.hpp" \
    --include-root "$include_root" --stdout >/dev/null

printf '%s\n' '#include <vector>' '#include "cloud/a.hpp"' \
    > "$sandbox/include/cloud/late_include.hpp"
if "$tool" --root "$sandbox/include/cloud/late_include.hpp" --include-root "$include_root" \
    --stdout >/dev/null 2>&1; then
    fail "late project include was accepted"
fi

if "$tool" --root "$root" --include-root "$include_root" --write --check \
    >/dev/null 2>&1; then
    fail "conflicting actions were accepted"
fi
