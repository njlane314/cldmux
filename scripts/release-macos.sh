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
minimum=${MACOS_MIN_VERSION:-13.0}
[[ $minimum =~ ^[0-9]+\.[0-9]+$ ]] || fail "MACOS_MIN_VERSION must look like 13.0"

for tool in codesign install lipo nm pkgbuild pkgutil shasum strip tar xcrun; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool is unavailable: $tool"
done

if [[ $mode == release ]]; then
    signing_identity=${MACOS_SIGN_IDENTITY:-}
    installer_identity=${MACOS_INSTALLER_IDENTITY:-}
    notary_profile=${MACOS_NOTARY_PROFILE:-}
    [[ -n $signing_identity && $signing_identity != - ]] ||
        fail "MACOS_SIGN_IDENTITY must name a Developer ID Application identity"
    [[ -n $installer_identity && $installer_identity != - ]] ||
        fail "MACOS_INSTALLER_IDENTITY must name a Developer ID Installer identity"
    [[ -n $notary_profile ]] || fail "MACOS_NOTARY_PROFILE must name a Keychain profile"
    [[ ! -e $output_dir ]] || fail "release output already exists: $output_dir"
fi

umask 077
work_parent="$root/build/release-work"
(cd "$root" && bash scripts/prepare-build.sh macos build build/release-work)
work=$(mktemp -d "$work_parent/macos.XXXXXX")
cleanup() {
    rm -rf -- "$work"
}
trap cleanup EXIT HUP INT TERM

sdk=$(xcrun --sdk macosx --show-sdk-path)
if [[ -n ${MACOS_RELEASE_CXX:-} ]]; then
    candidates=("$MACOS_RELEASE_CXX")
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
    -I"$root"
    "$root/apps/dispatch.cpp"
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
if nm -m "$universal" 2>/dev/null | grep -q 'non-external'; then
    fail "the release binary still contains local symbols"
fi
"$universal" --help >/dev/null

if [[ $mode == check ]]; then
    if [[ -e $output_dir ]]; then
        [[ -d $output_dir && ! -L $output_dir ]] ||
            fail "check output is not a plain directory: $output_dir"
        output_mode=$(stat -f '%Lp' "$output_dir")
        [[ $output_mode == 700 ]] ||
            fail "check output has mode $output_mode, expected 700"
    else
        mkdir -p "$output_dir"
    fi
    install -m 700 "$universal" "$output_dir/cldmux-dispatch"
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
    "$package_root/usr/local" "$package_root/usr/local/bin"
install -m 755 "$universal" "$package_root/usr/local/bin/cldmux-dispatch"
package="$work/cldmux-dispatch-$version.pkg"
pkgbuild --root "$package_root" \
    --identifier io.github.njlane314.cldmux-dispatch \
    --version "$version" --install-location / \
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
archive="$work/cldmux-dispatch-$version.tar.gz"
tar -C "$archive_root" -czf "$archive" cldmux-dispatch

output_parent=$(dirname -- "$output_dir")
mkdir -p "$output_parent"
mkdir "$output_dir" || fail "release output was created concurrently: $output_dir"
install -m 700 "$universal" "$output_dir/cldmux-dispatch"
install -m 600 "$package" "$output_dir/cldmux-dispatch-$version.pkg"
install -m 600 "$archive" "$output_dir/cldmux-dispatch-$version.tar.gz"
install -m 600 "$submission" "$output_dir/notarization.json"
install -m 600 "$work/notarization-log.json" "$output_dir/notarization-log.json"
(
    cd "$output_dir"
    shasum -a 256 "cldmux-dispatch-$version.pkg" >"cldmux-dispatch-$version.pkg.sha256"
    shasum -a 256 "cldmux-dispatch-$version.tar.gz" \
        >"cldmux-dispatch-$version.tar.gz.sha256"
)
