#!/bin/sh

set -eu

binary=$1
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

run() {
    env -i PATH="$PATH" \
        CLOUD_GCP_PROJECT=test-project \
        CLOUD_GCP_REGION=europe-west4 \
        CLOUD_AWS_JOB_QUEUE=test-queue \
        CLOUD_AWS_MACHINE_TYPE=m6i.xlarge \
        CLOUD_AWS_REGION=eu-west-1 \
        CLOUD_AZURE_BATCH_ENDPOINT=https://test.westeurope.batch.azure.com \
        CLOUD_AZURE_REGION=westeurope \
        "$binary" "$@"
}

output=$(run --help)
printf '%s\n' "$output" | grep -q '^Usage: cloud-empirical '
printf '%s\n' "$output" | grep -q '^  --provider=aws '

missing_history=$temporary/missing.history
output=$(run render-v1 \
    --candidates=gcp,aws,azure \
    --history="$missing_history" \
    --minimum-observations=1 \
    --expected-elapsed=60s \
    --hourly-quote=gcp:0.36 \
    --hourly-quote=aws:0.18 \
    --hourly-quote=azure:0.54 \
    --data-cost=gcp:0.01 \
    --data-cost=aws:0.02 \
    --data-cost=azure:0 \
    --no-catalogue)
printf '%s\n' "$output" | grep -q '^routing_basis=advisory-fallback$'
printf '%s\n' "$output" | grep -q \
    '^candidate_gcp_routing_runtime_source=caller-expected-fallback$'
printf '%s\n' "$output" | grep -q '^candidate_gcp_routing_quote_source=caller-override$'
printf '%s\n' "$output" | grep -q '^candidate_gcp_effective_cost_proxy_usd=0.016$'
printf '%s\n' "$output" | grep -q '^candidate_aws_effective_cost_proxy_usd=0.023$'
printf '%s\n' "$output" | grep -q '^candidate_azure_effective_cost_proxy_usd=0.009$'
printf '%s\n' "$output" | grep -q '^provider=azure$'
printf '%s\n' "$output" | grep -q '^status=dry-run$'
! printf '%s\n' "$output" | grep -q 'hourly cost unavailable'
! printf '%s\n' "$output" | grep -Ev '^[a-z_]+=.*$'
test ! -e "$missing_history"

output=$(run tie-v1 \
    --candidates=aws,gcp,azure \
    --history="$missing_history" \
    --expected-elapsed=60s \
    --hourly-quote=aws:0.36 \
    --hourly-quote=gcp:0.36 \
    --hourly-quote=azure:0.36 \
    --data-cost=aws:0 \
    --data-cost=gcp:0 \
    --data-cost=azure:0 \
    --no-catalogue)
printf '%s\n' "$output" | grep -q '^provider=aws$'

output=$(run override-v1 \
    --provider=aws \
    --history="$missing_history" \
    --hourly-quote=aws:0.18 \
    --data-cost=aws:0.02 \
    --no-catalogue)
printf '%s\n' "$output" | grep -q '^requested_provider=aws$'
printf '%s\n' "$output" | grep -q '^routing_basis=override$'
printf '%s\n' "$output" | grep -q '^provider=aws$'

history=$temporary/observations.history
printf '%s\n' \
    'history_version=1 workload=render-v2 provider=gcp region=europe-west4 machine=e2-standard-4 accelerator=none accelerator_count=0 spot=false requested_cpus=4 requested_memory_gb=16 observed_elapsed_seconds=240 succeeded=true recorded_at_unix_seconds=1' \
    'history_version=1 workload=render-v2 provider=gcp region=europe-west4 machine=e2-standard-4 accelerator=none accelerator_count=0 spot=false requested_cpus=4 requested_memory_gb=16 observed_elapsed_seconds=360 succeeded=true recorded_at_unix_seconds=1' \
    'history_version=1 workload=render-v2 provider=aws region=eu-west-1 machine=m6i.xlarge accelerator=none accelerator_count=0 spot=false requested_cpus=4 requested_memory_gb=16 observed_elapsed_seconds=30 succeeded=true recorded_at_unix_seconds=1' \
    'history_version=1 workload=render-v2 provider=aws region=eu-west-1 machine=m6i.xlarge accelerator=none accelerator_count=0 spot=false requested_cpus=4 requested_memory_gb=16 observed_elapsed_seconds=90 succeeded=true recorded_at_unix_seconds=1' \
    'history_version=1 workload=render-v2 provider=azure region=westeurope machine=Standard_D4s_v5 accelerator=none accelerator_count=0 spot=false requested_cpus=4 requested_memory_gb=16 observed_elapsed_seconds=60 succeeded=true recorded_at_unix_seconds=1' \
    'history_version=1 workload=render-v2 provider=azure region=westeurope machine=Standard_D4s_v5 accelerator=none accelerator_count=0 spot=false requested_cpus=4 requested_memory_gb=16 observed_elapsed_seconds=180 succeeded=true recorded_at_unix_seconds=1' \
    > "$history"
printf '%s\n' \
    'history_version=1 workload=render-v2 provider=aws region=eu-west-1 machine=m6i.xlarge accelerator=none accelerator_count=0 spot=false requested_cpus=4 requested_memory_gb=16 observed_elapsed_seconds=999 succeeded=false recorded_at_unix_seconds=1' \
    >> "$history"

before=$(cksum "$history")
output=$(run render-v2 \
    --candidates=gcp,aws,azure \
    --history="$history" \
    --minimum-observations=2 \
    --hourly-quote=gcp:0.36 \
    --hourly-quote=aws:0.36 \
    --hourly-quote=azure:0.36 \
    --data-cost=gcp:0 \
    --data-cost=aws:0.05 \
    --data-cost=azure:0 \
    --no-catalogue)
after=$(cksum "$history")
test "$before" = "$after"
printf '%s\n' "$output" | grep -q '^routing_basis=empirical$'
printf '%s\n' "$output" | grep -q '^candidate_gcp_observations=2$'
printf '%s\n' "$output" | grep -q \
    '^candidate_gcp_routing_runtime_source=historical-controller-wall-mean$'
printf '%s\n' "$output" | grep -q '^candidate_gcp_routing_runtime_seconds=300$'
printf '%s\n' "$output" | grep -q '^candidate_aws_routing_runtime_seconds=60$'
printf '%s\n' "$output" | grep -q '^candidate_azure_routing_runtime_seconds=120$'
printf '%s\n' "$output" | grep -q '^candidate_azure_effective_cost_proxy_usd=0.012$'
printf '%s\n' "$output" | grep -q '^provider=azure$'

output=$(run render-v2 \
    --candidates=gcp,aws,azure \
    --history="$history" \
    --minimum-observations=2 \
    --hourly-quote=gcp:0.36 \
    --hourly-quote=aws:0.36 \
    --hourly-quote=azure:0.36 \
    --data-cost=gcp:0 \
    --data-cost=aws:0 \
    --data-cost=azure:0.05 \
    --no-catalogue)
printf '%s\n' "$output" | grep -q '^candidate_aws_effective_cost_proxy_usd=0.006$'
printf '%s\n' "$output" | grep -q '^provider=aws$'

output=$(run render-v2 \
    --candidates=gcp,aws,azure \
    --history="$history" \
    --minimum-observations=3 \
    --expected-elapsed=30s \
    --hourly-quote=gcp:0.36 \
    --hourly-quote=aws:0.36 \
    --hourly-quote=azure:0.36 \
    --data-cost=gcp:0 \
    --data-cost=aws:0 \
    --data-cost=azure:0 \
    --no-catalogue)
printf '%s\n' "$output" | grep -q '^routing_basis=advisory-fallback$'
printf '%s\n' "$output" | grep -q '^candidate_gcp_observations=2$'
printf '%s\n' "$output" | grep -q '^candidate_gcp_routing_runtime_seconds=30$'
printf '%s\n' "$output" | grep -q '^candidate_aws_routing_runtime_seconds=30$'

malformed=$temporary/malformed.history
printf '%s\n' 'history_version=2 workload=broken' > "$malformed"
status=0
output=$(run broken-v1 \
    --candidates=gcp,aws \
    --history="$malformed" \
    --hourly-quote=gcp:0.1 \
    --hourly-quote=aws:0.1 \
    --data-cost=gcp:0 \
    --data-cost=aws:0 \
    --no-catalogue 2>&1) || status=$?
test "$status" -eq 2
printf '%s\n' "$output" | grep -q '^error=invalid empirical history at line 1:'

for invalid in --data-cost=gcp:-1 --hourly-quote=gcp:nan --hourly-quote=gcp:inf; do
    status=0
    run invalid-v1 --candidates=gcp,aws "$invalid" >/dev/null 2>&1 || status=$?
    test "$status" -eq 2
done

status=0
output=$(run missing-quote-v1 \
    --candidates=gcp,aws \
    --hourly-quote=gcp:0.1 \
    --data-cost=gcp:0 \
    --data-cost=aws:0 \
    --no-catalogue 2>&1) || status=$?
test "$status" -eq 2
test "$output" = 'error=no hourly quote supplied for candidate aws'

status=0
output=$(run missing-data-v1 \
    --candidates=gcp,aws \
    --hourly-quote=gcp:0.1 \
    --hourly-quote=aws:0.1 \
    --data-cost=gcp:0 \
    --no-catalogue 2>&1) || status=$?
test "$status" -eq 2
test "$output" = 'error=known data cost missing for candidate aws'

bad_machine=$(printf 'm6i.xlarge\ninjected=true')
status=0
output=$(env -i PATH="$PATH" \
    CLOUD_AWS_JOB_QUEUE=test-queue \
    CLOUD_AWS_MACHINE_TYPE="$bad_machine" \
    CLOUD_AWS_REGION=eu-west-1 \
    "$binary" unsafe-plan-v1 \
    --provider=aws \
    --history="$missing_history" \
    --hourly-quote=aws:0.1 \
    --data-cost=aws:0 \
    --no-catalogue 2>&1) || status=$?
test "$status" -eq 2
test "$output" = 'error=candidate aws machine cannot be recorded safely'

status=0
output=$(run preflight-v1 \
    --provider=aws \
    --history="$temporary/missing/observations.history" \
    --hourly-quote=aws:0.1 \
    --data-cost=aws:0 \
    --no-catalogue \
    --submit 2>&1) || status=$?
test "$status" -eq 2
printf '%s\n' "$output" | grep -q \
    '^error=cannot prepare empirical history for append:'
! printf '%s\n' "$output" | grep -q '^status=submitting$'

unterminated=$temporary/unterminated.history
printf '%s' \
    'history_version=1 workload=preflight-v1 provider=aws region=eu-west-1 machine=m6i.xlarge accelerator=none accelerator_count=0 spot=false requested_cpus=4 requested_memory_gb=16 observed_elapsed_seconds=10 succeeded=true recorded_at_unix_seconds=1' \
    > "$unterminated"
status=0
output=$(run preflight-v1 \
    --provider=aws \
    --history="$unterminated" \
    --minimum-observations=1 \
    --hourly-quote=aws:0.1 \
    --data-cost=aws:0 \
    --no-catalogue \
    --submit 2>&1) || status=$?
test "$status" -eq 2
printf '%s\n' "$output" | grep -q \
    '^error=empirical history must end with a newline before append:'
! printf '%s\n' "$output" | grep -q '^status=submitting$'
