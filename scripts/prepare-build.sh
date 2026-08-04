#!/usr/bin/env bash

set -euo pipefail

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

if [[ $# -ne 3 ]]; then
    fail "usage: prepare-build.sh PLATFORM BUILD_DIR CHECK_DIR"
fi

platform=$1
build_dir=$2
check_dir=$3
root=$(pwd -P)

validate_relative() {
    local name=$1
    local path=$2
    [[ -n $path && $path != /* ]] || fail "$name must be a non-empty relative path"
    [[ $path != *$'\n'* && $path != *$'\r'* ]] || fail "$name contains a newline"
    case "/$path/" in
    *//*|*/./*|*/../*) fail "$name must not contain empty, . or .. components" ;;
    esac
}

reject_symlinks() {
    local path=$1
    local current=$root
    local component
    local -a components
    IFS=/ read -r -a components <<<"$path"
    for component in "${components[@]}"; do
        current="$current/$component"
        [[ ! -L $current ]] || fail "build path traverses a symbolic link: $current"
    done
}

mode_of() {
    if [[ $platform == macos ]]; then
        stat -f '%Lp' "$1"
    else
        stat -c '%a' "$1"
    fi
}

ensure_private_dir() {
    local path=$1
    local label=$2
    reject_symlinks "$path"
    if [[ -e $root/$path ]]; then
        [[ -d $root/$path ]] || fail "$label is not a directory: $path"
    else
        mkdir -p -- "$root/$path"
    fi
    reject_symlinks "$path"
    if [[ $platform != windows ]]; then
        local mode
        mode=$(mode_of "$root/$path")
        [[ $mode == 700 ]] ||
            fail "$label has mode $mode, expected 700; refusing to change it"
    fi
}

validate_relative BUILD_DIR "$build_dir"
validate_relative CHECK_DIR "$check_dir"
case "/$build_dir/" in
*/.[gG][iI][tT]/*) fail "BUILD_DIR must not be inside .git" ;;
esac
[[ $check_dir == "$build_dir/"* ]] || fail "CHECK_DIR must be inside BUILD_DIR"

umask 077
ensure_private_dir "$build_dir" BUILD_DIR
ensure_private_dir "$check_dir" CHECK_DIR

build=$(cd -- "$root/$build_dir" && pwd -P)
check=$(cd -- "$root/$check_dir" && pwd -P)
case "$build/" in
"$root/"*) ;;
*) fail "BUILD_DIR resolves outside the repository" ;;
esac
case "$check/" in
"$build/"*) ;;
*) fail "CHECK_DIR resolves outside BUILD_DIR" ;;
esac
