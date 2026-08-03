#!/bin/sh

set -eu

binary=$1
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
physical_temporary=$(cd "$temporary" && pwd -P)

input=$temporary/case.tar.zst
output=$temporary/result.tar.zst
physical_output=$physical_temporary/result.tar.zst
digest=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
printf '%s\n' 'immutable input' > "$input"

run() {
    env -i PATH="$PATH" \
        DISPATCH_INPUT_ROOT=cloud://dispatch-input \
        DISPATCH_OUTPUT_ROOT=cloud://dispatch-output \
        CLOUD_GCP_PROJECT=test-project \
        CLOUD_GCP_REGION=europe-west4 \
        "$binary" "$@"
}

dry_run() {
    run \
        --id=simulation-0042 \
        --policy=gcp \
        --image="ghcr.io/acme/solver@sha256:$digest" \
        --input="$input" \
        --output="$output" \
        --no-catalogue \
        "$@" \
        -- /bin/echo hello
}

help=$(run --help)
printf '%s\n' "$help" | grep -q '^Usage: dispatch '
printf '%s\n' "$help" | grep -q '^  --submit '
printf '%s\n' "$help" | grep -q \
    '^Set distinct DISPATCH_INPUT_ROOT and DISPATCH_OUTPUT_ROOT cloud:// bucket/container$'
! printf '%s\n' "$help" | grep -q 'DISPATCH_ARTIFACT_ROOT'

before=$(cksum "$input")
quote=$(dry_run)
after=$(cksum "$input")
test "$before" = "$after"
printf '%s\n' "$quote" | grep -q '^output_version=1$'
printf '%s\n' "$quote" | grep -q '^program=dispatch$'
printf '%s\n' "$quote" | grep -q '^request_id=simulation-0042$'
printf '%s\n' "$quote" | grep -q '^requested_policy=gcp$'
printf '%s\n' "$quote" | grep -q '^provider=gcp$'
printf '%s\n' "$quote" | grep -q '^region=europe-west4$'
printf '%s\n' "$quote" | grep -q '^machine=e2-standard-4$'
printf '%s\n' "$quote" | grep -q '^hourly_rate_estimate_usd=unavailable$'
printf '%s\n' "$quote" | grep -q \
    '^estimated_compute_cost_for_expected_runtime_usd=unavailable$'
printf '%s\n' "$quote" | grep -q '^expected_active_runtime_seconds=300$'
printf '%s\n' "$quote" | grep -q '^controller_timeout_seconds=3600$'
printf '%s\n' "$quote" | grep -q '^configured_attempt_limit=2$'
printf '%s\n' "$quote" | grep -q \
    '^container_input=/dispatch/input/runs/simulation-0042/input.tar.zst$'
printf '%s\n' "$quote" | grep -q \
    '^container_output=/dispatch/output/runs/simulation-0042/output.tar.zst$'
printf '%s\n' "$quote" | grep -Fq "receipt_file=$physical_output.receipt"
printf '%s\n' "$quote" | grep -q '^approval=required$'
printf '%s\n' "$quote" | grep -q '^status=dry-run$'
! printf '%s\n' "$quote" | grep -Ev '^[a-z_]+=.*$'
test ! -e "$output"
test ! -e "$output.receipt"
test ! -e "$output.receipt.pending"

custom_receipt=$temporary/custom.receipt
physical_custom_receipt=$physical_temporary/custom.receipt
quote=$(dry_run --receipt="$custom_receipt" --cpus=8 --memory-gb=24 \
    --retries=0 --timeout=30m --expected-runtime=90s)
printf '%s\n' "$quote" | grep -q '^machine=e2-standard-8$'
printf '%s\n' "$quote" | grep -q '^expected_active_runtime_seconds=90$'
printf '%s\n' "$quote" | grep -q '^controller_timeout_seconds=1800$'
printf '%s\n' "$quote" | grep -q '^configured_attempt_limit=1$'
printf '%s\n' "$quote" | grep -Fq "receipt_file=$physical_custom_receipt"
test ! -e "$custom_receipt"

status=0
error=$(run \
    --id=bad/id \
    --policy=gcp \
    --image=image \
    --input="$input" \
    --output="$output" \
    --no-catalogue \
    -- /bin/echo 2>&1) || status=$?
test "$status" -eq 2
test "$error" = \
    'error=request id must contain 1-64 letters, digits, dots, underscores, or hyphens'

status=0
error=$(run \
    --id=missing-input \
    --policy=gcp \
    --image=image \
    --input="$temporary/missing" \
    --output="$output" \
    --no-catalogue \
    -- /bin/echo 2>&1) || status=$?
test "$status" -eq 2
test "$error" = 'error=input bundle must be an existing regular file'

status=0
error=$(run \
    --id=same-path \
    --policy=gcp \
    --image=image \
    --input="$input" \
    --output="$input" \
    --no-catalogue \
    -- /bin/echo 2>&1) || status=$?
test "$status" -eq 2
test "$error" = \
    'error=input, output, receipt, and pending receipt paths must be distinct'

mkdir "$temporary/real-parent"
ln -s "$temporary/real-parent" "$temporary/parent-alias"
status=0
error=$(run \
    --id=symlink-collision \
    --policy=gcp \
    --image=image \
    --input="$input" \
    --output="$temporary/real-parent/result" \
    --receipt="$temporary/parent-alias/result" \
    --no-catalogue \
    -- /bin/echo 2>&1) || status=$?
test "$status" -eq 2
test "$error" = \
    'error=input, output, receipt, and pending receipt paths must be distinct'

pending_collision=$temporary/pending-collision
status=0
error=$(run \
    --id=pending-collision \
    --policy=gcp \
    --image=image \
    --input="$input" \
    --output="$pending_collision.pending" \
    --receipt="$pending_collision" \
    --no-catalogue \
    -- /bin/echo 2>&1) || status=$?
test "$status" -eq 2
test "$error" = \
    'error=input, output, receipt, and pending receipt paths must be distinct'

printf '%s\n' existing > "$output"
status=0
error=$(dry_run 2>&1) || status=$?
test "$status" -eq 2
test "$error" = 'error=output bundle already exists'
rm "$output"

printf '%s\n' existing > "$output.receipt"
status=0
error=$(dry_run 2>&1) || status=$?
test "$status" -eq 2
test "$error" = 'error=receipt already exists'
rm "$output.receipt"

for arguments in \
    '--id=duplicate' \
    '--no-catalogue' \
    '--cpus=0' \
    '--memory-gb=nan' \
    '--timeout=0s' \
    '--expected-runtime=2h'
do
    status=0
    dry_run "$arguments" >/dev/null 2>&1 || status=$?
    test "$status" -eq 2
done

status=0
error=$(run \
    --id=cheapest-offline \
    --image=image \
    --input="$input" \
    --output="$output" \
    --no-catalogue \
    -- /bin/echo 2>&1) || status=$?
test "$status" -eq 2
test "$error" = 'error=cheapest policy requires catalogue pricing'

status=0
error=$(env -i PATH="$PATH" \
    CLOUD_GCP_PROJECT=test-project \
    CLOUD_GCP_REGION=europe-west4 \
    "$binary" \
    --id=missing-roots \
    --policy=gcp \
    --image=image \
    --input="$input" \
    --output="$output" \
    --no-catalogue \
    -- /bin/echo 2>&1) || status=$?
test "$status" -eq 2
test "$error" = \
    'error=distinct DISPATCH_INPUT_ROOT and DISPATCH_OUTPUT_ROOT values are required'

status=0
error=$(env -i PATH="$PATH" \
    DISPATCH_INPUT_ROOT=cloud://same-root \
    DISPATCH_OUTPUT_ROOT=cloud://same-root \
    CLOUD_GCP_PROJECT=test-project \
    CLOUD_GCP_REGION=europe-west4 \
    "$binary" \
    --id=same-roots \
    --policy=gcp \
    --image=image \
    --input="$input" \
    --output="$output" \
    --no-catalogue \
    -- /bin/echo 2>&1) || status=$?
test "$status" -eq 2
test "$error" = \
    'error=DISPATCH_INPUT_ROOT and DISPATCH_OUTPUT_ROOT must name different buckets or containers'

status=0
run --id=no-command --image=image --input="$input" --output="$output" \
    --no-catalogue >/dev/null 2>&1 || status=$?
test "$status" -eq 2

bad_option=$(printf '%s\n%s' --unknown injected=true)
status=0
error=$(run "$bad_option" 2>&1) || status=$?
test "$status" -eq 2
test "$(printf '%s\n' "$error" | wc -l | tr -d ' ')" -eq 1
printf '%s\n' "$error" | grep -Fq '\n'
