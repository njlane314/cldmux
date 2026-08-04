#!/usr/bin/env bash

set -euo pipefail

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

if [[ $# -ne 3 ]]; then
    fail "usage: release-macos.sh check|release VERSION OUTPUT_DIR"
fi

mode=$1
version=$2
output_dir=$3
case "$mode" in
check|release) ;;
*) fail "mode must be check or release" ;;
esac
[[ $version =~ ^[0-9]+(\.[0-9]+){1,3}([.-][0-9A-Za-z]+)*$ ]] ||
    fail "VERSION must be a dotted numeric release identifier"

root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir=${BUILD_DIR:-build}
minimum=${MACOS_MIN_VERSION:-13.0}
[[ $minimum =~ ^[0-9]+\.[0-9]+$ ]] || fail "MACOS_MIN_VERSION must look like 13.0"

validate_relative() {
    local name=$1
    local path=$2
    [[ -n $path && $path != /* ]] || fail "$name must be a non-empty relative path"
    [[ $path != *$'\n'* && $path != *$'\r'* ]] || fail "$name contains a newline"
    case "/$path/" in
    *//*|*/./*|*/../*) fail "$name must not contain empty, . or .. components" ;;
    esac
}

validate_relative BUILD_DIR "$build_dir"
validate_relative OUTPUT_DIR "$output_dir"
case "/$build_dir/" in
*/.[gG][iI][tT]/*) fail "BUILD_DIR must not be inside .git" ;;
esac
[[ $output_dir == "$build_dir/"* ]] || fail "OUTPUT_DIR must be inside BUILD_DIR"
output_parent=${output_dir%/*}

source_version=$(sed -n \
    's/^#define CLDMUX_VERSION "\([^"]*\)"$/\1/p' \
    "$root/include/cldmux/detail/config.hpp")
generated_version=$(sed -n \
    's/^#define CLDMUX_VERSION "\([^"]*\)"$/\1/p' \
    "$root/cldmux")
[[ $source_version =~ ^[0-9]+(\.[0-9]+){1,3}([.-][0-9A-Za-z]+)*$ ]] ||
    fail "source CLDMUX_VERSION is missing or invalid"
[[ $generated_version == "$source_version" ]] ||
    fail "source and generated cldmux versions do not match"

for tool in codesign git install lipo nm pkgbuild pkgutil plutil shasum spctl strip tar xcrun; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool is unavailable: $tool"
done

source_status() {
    git -C "$root" status --porcelain --untracked-files=no || return
    git -C "$root" ls-files --others --exclude-standard -- . \
        ":(top,exclude,literal)$build_dir"
}

tracked_build=$(git -C "$root" ls-files -- ":(top,literal)$build_dir") ||
    fail "cannot inspect BUILD_DIR"
[[ -z $tracked_build ]] || fail "BUILD_DIR must not contain tracked source"

source_commit=
if [[ $mode == release ]]; then
    [[ $version == "$source_version" ]] ||
        fail "release version $version does not match cldmux $source_version"
    signing_identity=${MACOS_SIGN_IDENTITY:-}
    installer_identity=${MACOS_INSTALLER_IDENTITY:-}
    notary_profile=${MACOS_NOTARY_PROFILE:-}
    [[ -n $signing_identity && $signing_identity != - ]] ||
        fail "MACOS_SIGN_IDENTITY must name a Developer ID Application identity"
    [[ -n $installer_identity && $installer_identity != - ]] ||
        fail "MACOS_INSTALLER_IDENTITY must name a Developer ID Installer identity"
    [[ -n $notary_profile ]] || fail "MACOS_NOTARY_PROFILE must name a Keychain profile"
    source_commit=$(git -C "$root" rev-parse --verify HEAD) ||
        fail "release source is not a Git commit"
    [[ ${SOURCE_COMMIT:-$source_commit} == "$source_commit" ]] ||
        fail "SOURCE_COMMIT does not match the checked-out commit"
    status=$(source_status) || fail "cannot inspect the release source tree"
    [[ -z $status ]] ||
        fail "release source tree is not clean"
    [[ -f $root/LICENSE ]] || fail "LICENSE is missing"
fi

umask 077
work_dir="$build_dir/release-work"
(cd "$root" && bash scripts/prepare-build.sh macos "$build_dir" "$work_dir")
(cd "$root" && bash scripts/prepare-build.sh macos "$build_dir" "$output_parent")
work_parent="$root/$work_dir"
work=$(mktemp -d "$work_parent/macos.XXXXXX")
output_path="$root/$output_dir"
reserved=false
cleanup() {
    local status=$?
    trap - EXIT HUP INT TERM
    rm -rf -- "$work"
    if [[ $reserved == true ]]; then
        rmdir -- "$output_path" 2>/dev/null || true
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

source_root=$root
if [[ $mode == check ]]; then
    (cd "$root" && bash scripts/prepare-build.sh macos "$build_dir" "$output_dir")
else
    mkdir "$output_path" || fail "release output already exists or is reserved: $output_dir"
    reserved=true
    printf '%s\n' "$source_commit" >"$work/SOURCE_COMMIT"
    install -d -m 700 "$work/source"
    git -C "$root" archive --format=tar --output="$work/source.tar" "$source_commit" ||
        fail "cannot snapshot the release source commit"
    tar -C "$work/source" -xf "$work/source.tar"
    source_root="$work/source"
    archived_source_version=$(sed -n \
        's/^#define CLDMUX_VERSION "\([^"]*\)"$/\1/p' \
        "$source_root/include/cldmux/detail/config.hpp")
    archived_generated_version=$(sed -n \
        's/^#define CLDMUX_VERSION "\([^"]*\)"$/\1/p' \
        "$source_root/cldmux")
    [[ $archived_source_version == "$version" && \
        $archived_generated_version == "$version" ]] ||
        fail "archived source does not match release version $version"
    install -m 600 "$source_root/LICENSE" "$work/LICENSE"
fi

sdk=$(xcrun --sdk macosx --show-sdk-path)
if [[ -n ${MACOS_RELEASE_CXX:-} ]]; then
    resolved=$(command -v -- "$MACOS_RELEASE_CXX" 2>/dev/null || true)
    candidates=("${resolved:-$MACOS_RELEASE_CXX}")
else
    candidates=(
        "$(xcrun --sdk macosx --find clang++)"
        /opt/homebrew/opt/llvm/bin/clang++
        /usr/local/opt/llvm/bin/clang++
    )
fi
cxx=
for candidate in "${candidates[@]}"; do
    if [[ -x $candidate ]] &&
        printf '#include <chrono>\n' | "$candidate" -isysroot "$sdk" \
            -x c++ -fsyntax-only - >/dev/null 2>&1; then
        cxx=$candidate
        break
    fi
done
[[ -n $cxx ]] || fail "no macOS C++17 compiler with libc++ headers is available"
common=(
    -std=c++17
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror
    -O2 -DNDEBUG -fvisibility=hidden -fvisibility-inlines-hidden
    -Wl,-dead_strip
    -isysroot "$sdk"
    "-mmacosx-version-min=$minimum"
    -I"$source_root"
    "$source_root/apps/dispatch.cpp"
    -lcurl -pthread
)

for architecture in arm64 x86_64; do
    "$cxx" "${common[@]}" -arch "$architecture" -o "$work/cldmux-dispatch-$architecture"
    lipo "$work/cldmux-dispatch-$architecture" -verify_arch "$architecture"
done

universal="$work/cldmux-dispatch"
lipo -create "$work/cldmux-dispatch-arm64" "$work/cldmux-dispatch-x86_64" \
    -output "$universal"
lipo "$universal" -verify_arch arm64 x86_64

# The signature must cover the final bytes. Nothing may mutate this file after
# strip and before the one universal signature below.
strip -S -x "$universal"
if [[ $mode == check ]]; then
    codesign --force --sign - --identifier io.github.njlane314.cldmux-dispatch \
        --options runtime "$universal"
else
    codesign --force --sign "$signing_identity" \
        --identifier io.github.njlane314.cldmux-dispatch \
        --options runtime --timestamp "$universal"
fi

codesign --verify --strict --all-architectures --verbose=2 "$universal"
lipo "$universal" -verify_arch arm64 x86_64
nm -m "$universal" >"$work/symbols.txt"
if grep -q 'non-external' "$work/symbols.txt"; then
    fail "the release binary still contains local symbols"
fi
"$universal" --help >/dev/null

if [[ $mode == check ]]; then
    install -m 700 "$universal" "$output_path/cldmux-dispatch"
    exit 0
fi

signature=$(codesign -dv --verbose=4 "$universal" 2>&1)
[[ $signature == *"Authority=Developer ID Application:"* ]] ||
    fail "the binary is not signed by a Developer ID Application certificate"
[[ $signature != *"Signature=adhoc"* && $signature != *"TeamIdentifier=not set"* ]] ||
    fail "the binary has no production signing identity"
[[ $signature == *runtime* ]] || fail "the hardened runtime flag is missing"
[[ $signature == *Timestamp=* ]] || fail "the secure signing timestamp is missing"

package_root="$work/package-root"
install -d -m 755 "$package_root" "$package_root/usr" \
    "$package_root/usr/local" "$package_root/usr/local/bin" \
    "$package_root/usr/local/share" "$package_root/usr/local/share/doc" \
    "$package_root/usr/local/share/doc/cldmux"
install -m 755 "$universal" "$package_root/usr/local/bin/cldmux-dispatch"
install -m 644 "$work/LICENSE" "$package_root/usr/local/share/doc/cldmux/LICENSE"
install -m 644 "$work/SOURCE_COMMIT" \
    "$package_root/usr/local/share/doc/cldmux/SOURCE_COMMIT"
package="$work/cldmux-dispatch-$version.pkg"
pkgbuild --root "$package_root" \
    --identifier io.github.njlane314.cldmux-dispatch \
    --version "$version" --min-os-version "$minimum" --install-location / \
    --sign "$installer_identity" "$package"

package_signature=$(pkgutil --check-signature "$package" 2>&1)
[[ $package_signature == *"Developer ID Installer:"* ]] ||
    fail "the package is not signed by a Developer ID Installer certificate"

submission="$work/notarization.json"
notary_args=(--keychain-profile "$notary_profile")
if [[ -n ${MACOS_NOTARY_KEYCHAIN:-} ]]; then
    notary_args+=(--keychain "$MACOS_NOTARY_KEYCHAIN")
fi
xcrun notarytool submit "$package" "${notary_args[@]}" \
    --wait --output-format json >"$submission"
status=$(/usr/bin/plutil -extract status raw -o - "$submission")
submission_id=$(/usr/bin/plutil -extract id raw -o - "$submission")
[[ $status == Accepted ]] || fail "Apple notarization returned: $status"
xcrun notarytool log "$submission_id" "${notary_args[@]}" \
    >"$work/notarization-log.json"

xcrun stapler staple "$package"
xcrun stapler validate "$package"
spctl --assess --type install --verbose=2 "$package"
codesign --verify --strict --all-architectures --verbose=2 "$universal"
codesign -vvvv -R='notarized' --check-notarization "$universal"

archive_root="$work/archive-root"
install -d -m 700 "$archive_root"
install -m 755 "$universal" "$archive_root/cldmux-dispatch"
install -m 644 "$work/LICENSE" "$archive_root/LICENSE"
install -m 644 "$work/SOURCE_COMMIT" "$archive_root/SOURCE_COMMIT"
archive="$work/cldmux-dispatch-$version.tar.gz"
tar -C "$archive_root" -czf "$archive" cldmux-dispatch LICENSE SOURCE_COMMIT

: >"$output_path/.incomplete"
install -m 700 "$universal" "$output_path/cldmux-dispatch"
install -m 600 "$package" "$output_path/cldmux-dispatch-$version.pkg"
install -m 600 "$archive" "$output_path/cldmux-dispatch-$version.tar.gz"
install -m 600 "$work/LICENSE" "$output_path/LICENSE"
install -m 600 "$work/SOURCE_COMMIT" "$output_path/SOURCE_COMMIT"
install -m 600 "$submission" "$output_path/notarization.json"
install -m 600 "$work/notarization-log.json" "$output_path/notarization-log.json"
(
    cd "$output_path"
    shasum -a 256 "cldmux-dispatch-$version.pkg" >"cldmux-dispatch-$version.pkg.sha256"
    shasum -a 256 "cldmux-dispatch-$version.tar.gz" \
        >"cldmux-dispatch-$version.tar.gz.sha256"
)
rm -- "$output_path/.incomplete"
reserved=false
