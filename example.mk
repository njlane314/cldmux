# CLDMUX MAKE PIPELINE EXAMPLE
#
# This is the Make counterpart to example.cpp: a deliberately verbose, runnable
# description of the decisions behind a small file-based workflow. Invoke it
# from the repository root so the source prerequisites below resolve correctly:
#
#   make -f example.mk help
#
# Make is the DAG controller. It decides which nodes are stale, starts independent
# recipes in parallel, and does not release a downstream node until every
# prerequisite has succeeded. Docker or GCP Batch executes one processing node;
# neither of them needs to understand the complete graph.
#
# The example graph is:
#
#   example.cpp -> code-input.tar.zst -> scan-code.tar.zst --+
#                                                               |
#   README.md ----> docs-input.tar.zst -> scan-docs.tar.zst --+  |
#                                                            |  |
#                                                            v  v
#                                                     combine-input.tar.zst
#                                                               |
#                                                               v
#                                                        combined.tar.zst
#                                                               |
#                                                               v
#                                                         report.tar.zst
#
# scan-code and scan-docs have no dependency on one another, so `make -j2` may
# execute them concurrently. combine waits for both, and report waits for
# combine. A recipe failure leaves its downstream half of the graph untouched.
#
# Every processing node uses the same worker contract:
#
#   /app/pipeline-worker STAGE INPUT_BUNDLE OUTPUT_BUNDLE
#
# The image author supplies that small application. It should extract its input
# into container-local scratch, run the named scientific/data operation, package
# one immutable output bundle, and return nonzero on failure. It must not submit
# jobs or know about Make. The four STAGE values used here are scan-code,
# scan-docs, combine, and report.
#
# The example gives that worker stable input names: scan-code receives an archive
# containing `example.cpp`, scan-docs receives `README.md`, and combine receives
# `scan-code.tar.zst` plus `scan-docs.tar.zst`. Report consumes combine's output
# bundle directly. The worker owns the internal schema of every output bundle.
#
# This file supports GNU Make 3.81 so it also parses with macOS /usr/bin/make.
# Homebrew GNU Make 4.x (`gmake`) is preferable for `--output-sync=target` when
# several local containers or cloud jobs emit logs concurrently.
# Host-side bundle creation uses `tar` and `zstd` in both execution modes.

.DEFAULT_GOAL := help

# WORKFLOW IDENTITY
#
# Make notices file timestamps, not changes to recipe text, image variables, or
# numerical environments. Treat PIPELINE_ID as a small explicit cache/version
# key. Change it whenever the image, worker argv, algorithm, seed, architecture,
# or other result-affecting choice changes. A production runner would derive a
# content hash instead of relying on a hand-maintained value.
#
# Dispatch request IDs accept only 1-64 letters, digits, dots, underscores, or
# hyphens. This example reserves enough characters for its longest stage suffix.
PIPELINE_ID ?= example-v1

# EXECUTION CHOICE
#
# `local` reparses this file with BACKEND=local and runs each processing node in
# Docker. `cloud` reparses it with BACKEND=gcp and sends each node through the
# existing cldmux dispatcher. Separate result directories prevent a completed
# local artifact from accidentally satisfying a cloud target, or vice versa.
BACKEND ?= local
WORK_ROOT ?= build/pipeline-example

# Use one digest-pinned OCI image in both places. The placeholder is sufficient
# for `help`, `dag`, and the non-mutating cloud plan, but local/cloud execution
# refuses it. Tags such as `:latest` are intentionally rejected because they do
# not identify the program that produced an artifact.
IMAGE ?= example.invalid/pipeline-worker@sha256:0000000000000000000000000000000000000000000000000000000000000000
PLATFORM ?= linux/amd64
CONTAINER ?= docker

# Match these values in local and cloud modes when CPU count, memory, threading,
# or hardware can change numerical results. Docker uses MEMORY_GB as a hard
# container limit; cldmux treats both values as portable minimum resources.
CPUS ?= 2
MEMORY_GB ?= 4

# CLOUD POLICY
#
# Cloud work is deliberately GCP-only here so the example has one exact worker
# entrypoint contract. cldmux itself remains provider-neutral. The dispatcher is
# built by the repository Makefile when it is missing.
DISPATCH ?= /tmp/cldmux-dispatch
APPROVE_CLOUD ?= NO
ALLOW_UNPRICED ?= NO
MAX_HOURLY_USD ?=
CLOUD_EXPECTED_RUNTIME ?= 5m
CLOUD_TIMEOUT ?= 30m
CLOUD_RETRIES ?= 1

# PATHS ARE THE GRAPH
#
# Ordinary files, not phony stage names, carry freshness through the DAG. Each
# worker has one bundle target. Its `.receipt` neighbour exists only for a cloud
# run and is audit/recovery evidence rather than an independently produced node.
INPUT_DIR := $(WORK_ROOT)/$(PIPELINE_ID)/inputs
RESULT_DIR := $(WORK_ROOT)/$(PIPELINE_ID)/$(BACKEND)
PLAN_DIR := $(WORK_ROOT)/$(PIPELINE_ID)/plans

CODE_INPUT := $(INPUT_DIR)/code-input.tar.zst
DOCS_INPUT := $(INPUT_DIR)/docs-input.tar.zst
CODE_SCAN := $(RESULT_DIR)/scan-code.tar.zst
DOCS_SCAN := $(RESULT_DIR)/scan-docs.tar.zst
COMBINE_INPUT := $(INPUT_DIR)/$(BACKEND)-combine-input.tar.zst
COMBINED := $(RESULT_DIR)/combined.tar.zst
REPORT := $(RESULT_DIR)/report.tar.zst
STAGE_OUTPUTS := $(CODE_SCAN) $(DOCS_SCAN) $(COMBINED) $(REPORT)

MAKEFILE_SELF := $(lastword $(MAKEFILE_LIST))

.PHONY: help dag local cloud cloud-plan cloud-plan-code cloud-plan-docs \
	id-ready packaging-ready local-ready cloud-ready verify-code verify-docs \
	verify-combined verify-report pipeline check

# A signal arriving just after dispatch has materialised an output must not make
# Make delete that verified artifact. Failed cloud transactions may also leave
# `.pending` recovery records beside it; those records must be inspected rather
# than casually removed.
.PRECIOUS: $(STAGE_OUTPUTS)

help:
	@printf '%s\n' \
		'CLDMUX Make DAG example' \
		'' \
		'Inspect the graph:' \
		'  make -f example.mk dag' \
		'' \
		'Run the worker image locally (independent scans may run in parallel):' \
		'  make -f example.mk -j2 local IMAGE=REGISTRY/WORKER@sha256:DIGEST' \
		'' \
		'Plan the first two GCP nodes without provider credentials or remote mutation:' \
		'  make -f example.mk -j2 cloud-plan' \
		'' \
		'Run the complete DAG through GCP Batch:' \
		'  export CLDMUX_GCP_PROJECT=PROJECT' \
		'  export CLDMUX_GCP_REGION=europe-west4' \
		'  export DISPATCH_INPUT_ROOT=cloud://IMMUTABLE-INPUT-BUCKET' \
		'  export DISPATCH_OUTPUT_ROOT=cloud://CREATE-ONLY-OUTPUT-BUCKET' \
		'  make -f example.mk -j2 cloud APPROVE_CLOUD=YES PIPELINE_ID=RUN-VERSION \' \
		'      IMAGE=REGISTRY/WORKER@sha256:DIGEST' \
		'' \
		'Cloud mutation requires APPROVE_CLOUD=YES. If catalogue pricing is' \
		'unavailable, ALLOW_UNPRICED=YES is a second explicit approval.'

dag:
	@printf '%s\n' \
		'example.cpp -> code input -> scan-code --+' \
		'                                          +-> combine -> report' \
		'README.md  -> docs input -> scan-docs --+'

# AGGREGATE TARGETS
#
# These wrappers re-enter the file with a fixed backend. The recipes and graph
# below remain identical; only RUN_STAGE changes. The `+` lets GNU Make share its
# jobserver with the recursive invocation, so `-jN` remains the concurrency cap.
local:
	+$(MAKE) --no-print-directory -f "$(MAKEFILE_SELF)" pipeline BACKEND=local

cloud:
	+$(MAKE) --no-print-directory -f "$(MAKEFILE_SELF)" pipeline BACKEND=gcp

pipeline: verify-report
	@printf 'pipeline=%s\nresult=%s\n' "$(BACKEND)" "$(REPORT)"

# LOCAL PACKAGING NODES
#
# cldmux dispatch deliberately transports already-formed bundles; it is not an
# archiver. These host-side rules make that boundary visible. Temporary files
# plus rename mean Make never observes a half-written bundle. Real pipelines
# should also normalise archive metadata when byte-for-byte reproducibility is
# required.
$(INPUT_DIR) $(RESULT_DIR) $(PLAN_DIR): | id-ready
	@mkdir -p "$@"

$(CODE_INPUT): example.cpp | $(INPUT_DIR) packaging-ready
	@set -eu; \
		test ! -e "$@" || { \
			printf '%s\n' 'input bundle is immutable; choose a new PIPELINE_ID' >&2; exit 1; \
		}; \
		archive="$@.tar.tmp"; compressed="$@.tmp"; \
		trap 'rm -f "$$archive" "$$compressed"' 0 1 2 3 15; \
		tar -cf "$$archive" "$<"; \
		zstd -q -f "$$archive" -o "$$compressed"; \
		mv "$$compressed" "$@"; \
		rm -f "$$archive"

$(DOCS_INPUT): README.md | $(INPUT_DIR) packaging-ready
	@set -eu; \
		test ! -e "$@" || { \
			printf '%s\n' 'input bundle is immutable; choose a new PIPELINE_ID' >&2; exit 1; \
		}; \
		archive="$@.tar.tmp"; compressed="$@.tmp"; \
		trap 'rm -f "$$archive" "$$compressed"' 0 1 2 3 15; \
		tar -cf "$$archive" "$<"; \
		zstd -q -f "$$archive" -o "$$compressed"; \
		mv "$$compressed" "$@"; \
		rm -f "$$archive"

# Fan-in needs one transport object, so Make packages both independent scan
# products after they have completed. A more advanced content-addressed runner
# could upload a manifest of immutable blobs instead of nesting archives.
$(COMBINE_INPUT): $(CODE_SCAN) $(DOCS_SCAN) | $(INPUT_DIR) packaging-ready verify-code verify-docs
	@set -eu; \
		test ! -e "$@" || { \
			printf '%s\n' 'fan-in bundle is immutable; choose a new PIPELINE_ID' >&2; exit 1; \
		}; \
		archive="$@.tar.tmp"; compressed="$@.tmp"; \
		contents=$$(mktemp -d "$(INPUT_DIR)/combine.XXXXXX"); \
		trap 'rm -rf "$$contents"; rm -f "$$archive" "$$compressed"' 0 1 2 3 15; \
		cp "$(CODE_SCAN)" "$$contents/scan-code.tar.zst"; \
		cp "$(DOCS_SCAN)" "$$contents/scan-docs.tar.zst"; \
		tar -cf "$$archive" -C "$$contents" scan-code.tar.zst scan-docs.tar.zst; \
		zstd -q -f "$$archive" -o "$$compressed"; \
		mv "$$compressed" "$@"; \
		rm -rf "$$contents"; \
		rm -f "$$archive"

# VALIDATION NODES
#
# These are order-only prerequisites: checking Docker, credentials, or the
# dispatcher should not by itself make an immutable scientific output stale.
# Result-affecting changes belong in a new PIPELINE_ID.
define CHECK_ID
	id="$(PIPELINE_ID)"; \
	case "$$id" in ''|.|..|*[!A-Za-z0-9._-]*) \
		printf '%s\n' 'PIPELINE_ID must be safe, nonempty, and different from . and ..' >&2; \
		exit 2;; \
	esac; \
	test "$${#id}" -le 48 || { \
		printf '%s\n' 'PIPELINE_ID must be at most 48 characters in this example' >&2; \
		exit 2; \
	}
endef

id-ready:
	@set -eu; $(CHECK_ID)

packaging-ready:
	@command -v tar >/dev/null 2>&1 || { \
		printf '%s\n' 'host-side packaging requires tar' >&2; exit 2; \
	}
	@command -v zstd >/dev/null 2>&1 || { \
		printf '%s\n' 'host-side packaging requires zstd' >&2; exit 2; \
	}

define CHECK_IMAGE
	image="$(IMAGE)"; digest=$${image##*@sha256:}; \
	case "$$image" in example.invalid/*) \
		printf '%s\n' 'replace IMAGE with the digest-pinned pipeline-worker image' >&2; \
		exit 2;; \
	esac; \
	test "$$digest" != "$$image" && test "$${#digest}" -eq 64 || { \
		printf '%s\n' 'IMAGE must end in @sha256: followed by 64 hexadecimal digits' >&2; \
		exit 2; \
	}; \
	case "$$digest" in *[!0-9A-Fa-f]*) \
		printf '%s\n' 'IMAGE digest contains a non-hexadecimal character' >&2; \
		exit 2;; \
	esac
endef

local-ready: id-ready
	@set -eu; \
		$(CHECK_IMAGE); \
		command -v "$(CONTAINER)" >/dev/null 2>&1 || { \
			printf '%s\n' 'local execution requires Docker or a compatible CONTAINER command' >&2; \
			exit 2; \
		}

cloud-ready: id-ready
	@set -eu; \
		$(CHECK_IMAGE); \
		test "$(APPROVE_CLOUD)" = YES || { \
			printf '%s\n' 'cloud execution requires the exact approval APPROVE_CLOUD=YES' >&2; \
			exit 2; \
		}; \
		case "$(ALLOW_UNPRICED)" in YES|NO) ;; *) \
			printf '%s\n' 'ALLOW_UNPRICED must be YES or NO' >&2; exit 2;; \
		esac; \
		test -n "$${CLDMUX_GCP_PROJECT:-}" || { \
			printf '%s\n' 'CLDMUX_GCP_PROJECT is required' >&2; exit 2; \
		}; \
		test -n "$${CLDMUX_GCP_REGION:-}" || { \
			printf '%s\n' 'CLDMUX_GCP_REGION is required' >&2; exit 2; \
		}; \
		test -n "$${DISPATCH_INPUT_ROOT:-}" || { \
			printf '%s\n' 'DISPATCH_INPUT_ROOT is required' >&2; exit 2; \
		}; \
		test -n "$${DISPATCH_OUTPUT_ROOT:-}" || { \
			printf '%s\n' 'DISPATCH_OUTPUT_ROOT is required' >&2; exit 2; \
		}; \
		test "$$DISPATCH_INPUT_ROOT" != "$$DISPATCH_OUTPUT_ROOT" || { \
			printf '%s\n' 'dispatch input and output roots must differ' >&2; exit 2; \
		}

# Every cloud edge is verified before a paid downstream edge may start. Merely
# finding an output file is insufficient: its receipt must describe a complete,
# successful transaction for this request/image, and its SHA-256 must match the
# bytes Make will package or pass onward. These phony checks are order-only
# prerequisites, so auditing them does not make an otherwise current target
# stale. Local mode has no dispatch receipt and checks only materialisation.
define VERIFY_STAGE
	@set -eu; \
		product="$(1)"; \
		test -f "$$product" || { \
			printf '%s\n' "missing stage output: $$product" >&2; exit 1; \
		}; \
		if test "$(BACKEND)" = gcp; then \
			receipt="$$product.receipt"; \
			test -f "$$receipt" || { \
				printf '%s\n' "missing cloud receipt: $$receipt" >&2; exit 1; \
			}; \
			grep -Fqx 'receipt_status=complete' "$$receipt"; \
			grep -Fqx 'success=true' "$$receipt"; \
			grep -Fqx 'job_succeeded=true' "$$receipt"; \
			grep -Fqx 'output_retrieved=true' "$$receipt"; \
			grep -Fqx 'receipt_persisted=true' "$$receipt"; \
			grep -Fqx 'request_id=$(PIPELINE_ID)-$(2)' "$$receipt"; \
			grep -Fqx 'image_reference=$(IMAGE)' "$$receipt"; \
			expected=$$(sed -n 's/^output_sha256=//p' "$$receipt"); \
			if command -v sha256sum >/dev/null 2>&1; then \
				actual=$$(sha256sum "$$product" | awk '{print $$1}'); \
			elif command -v shasum >/dev/null 2>&1; then \
				actual=$$(shasum -a 256 "$$product" | awk '{print $$1}'); \
			else \
				printf '%s\n' 'receipt verification requires sha256sum or shasum' >&2; exit 2; \
			fi; \
			test -n "$$expected" && test "$$actual" = "$$expected" || { \
				printf '%s\n' "cloud output hash does not match $$receipt" >&2; exit 1; \
			}; \
		fi
endef

verify-code: $(CODE_SCAN)
	$(call VERIFY_STAGE,$(CODE_SCAN),scan-code)

verify-docs: $(DOCS_SCAN)
	$(call VERIFY_STAGE,$(DOCS_SCAN),scan-docs)

verify-combined: $(COMBINED)
	$(call VERIFY_STAGE,$(COMBINED),combine)

verify-report: $(REPORT)
	$(call VERIFY_STAGE,$(REPORT),report)

# Building this target uses the repository's ordinary build rules. Applications
# may instead install cldmux-dispatch elsewhere and override DISPATCH.
$(DISPATCH): apps/dispatch.cpp apps/dispatch.hpp apps/dispatch_main.cpp cldmux Makefile
	+$(MAKE) -f Makefile dispatch DISPATCH="$@"

# EXECUTION ADAPTER
#
# Local mode mirrors dispatch's fixed mount layout. It snapshots the input into a
# private stage directory, runs the same image/entrypoint used by Batch, verifies
# that the worker produced its promised bundle, then atomically materialises the
# Make target. `--platform` avoids silently comparing native ARM output on this
# Mac with ordinary x86-64 Batch output; change it only with PIPELINE_ID.
ifeq ($(BACKEND),local)
RUNNER_READY := local-ready

define RUN_STAGE
	@set -eu; \
		id="$(PIPELINE_ID)-$(1)"; \
		input_root="$(abspath $(RESULT_DIR)/.mounts/$(1)/input)"; \
		output_root="$(abspath $(RESULT_DIR)/.mounts/$(1)/output)"; \
		remote_input="$$input_root/runs/$$id/input.tar.zst"; \
		remote_output="$$output_root/runs/$$id/output.tar.zst"; \
		temporary="$@.tmp"; \
		trap 'rm -f "$$temporary"' 0 1 2 3 15; \
		test ! -e "$@" || { \
			printf '%s\n' 'local result is immutable; choose a new PIPELINE_ID' >&2; exit 1; \
		}; \
		mkdir -p "$$(dirname "$$remote_input")" "$$(dirname "$$remote_output")"; \
		cp "$<" "$$remote_input"; \
		rm -f "$$remote_output"; \
		"$(CONTAINER)" run --rm \
			--platform="$(PLATFORM)" \
			--cpus="$(CPUS)" --memory="$(MEMORY_GB)g" \
			--mount "type=bind,src=$$input_root,dst=/dispatch/input,readonly" \
			--mount "type=bind,src=$$output_root,dst=/dispatch/output" \
			--entrypoint=/app/pipeline-worker \
			"$(IMAGE)" "$(1)" \
			"/dispatch/input/runs/$$id/input.tar.zst" \
			"/dispatch/output/runs/$$id/output.tar.zst"; \
		test -f "$$remote_output" || { \
			printf '%s\n' "worker did not produce $$remote_output" >&2; exit 1; \
		}; \
		cp "$$remote_output" "$$temporary"; \
		mv "$$temporary" "$@"
endef

else ifeq ($(BACKEND),gcp)
RUNNER_READY := $(DISPATCH) cloud-ready

# Cloud mode delegates one complete upload -> submit -> wait -> download
# transaction to dispatch. It blocks until both `$@` and `$@.receipt` are local,
# so Make cannot start a dependent stage early. GCP Batch owns VM provisioning
# and configured attempt retries; Make owns only DAG-level scheduling.
define RUN_STAGE
	@set -eu; \
		unpriced=''; \
		if test "$(ALLOW_UNPRICED)" = YES; then unpriced=--allow-unpriced; fi; \
		"$(DISPATCH)" \
			--id="$(PIPELINE_ID)-$(1)" \
			--policy=gcp \
			--image="$(IMAGE)" \
			--input="$<" --output="$@" \
			--cpus="$(CPUS)" --memory-gb="$(MEMORY_GB)" \
			--retries="$(CLOUD_RETRIES)" --timeout="$(CLOUD_TIMEOUT)" \
			--expected-runtime="$(CLOUD_EXPECTED_RUNTIME)" \
			$(if $(strip $(MAX_HOURLY_USD)),--max-hourly-usd="$(MAX_HOURLY_USD)",) \
			$$unpriced --submit \
			-- /app/pipeline-worker "$(1)" \
			"/dispatch/input/runs/$(PIPELINE_ID)-$(1)/input.tar.zst" \
			"/dispatch/output/runs/$(PIPELINE_ID)-$(1)/output.tar.zst"; \
		test -f "$@" && test -f "$@.receipt"
endef

else
$(error BACKEND must be local or gcp)
endif

# PROCESSING NODES
$(CODE_SCAN): $(CODE_INPUT) | $(RESULT_DIR) $(RUNNER_READY)
	$(call RUN_STAGE,scan-code)

$(DOCS_SCAN): $(DOCS_INPUT) | $(RESULT_DIR) $(RUNNER_READY)
	$(call RUN_STAGE,scan-docs)

$(COMBINED): $(COMBINE_INPUT) | $(RESULT_DIR) $(RUNNER_READY) verify-code verify-docs
	$(call RUN_STAGE,combine)

$(REPORT): $(COMBINED) | $(RESULT_DIR) $(RUNNER_READY) verify-combined
	$(call RUN_STAGE,report)

# READ-ONLY CLOUD PLANNING
#
# A dispatch dry run returns success but intentionally creates no target, so it
# must never be used as the recipe for CODE_SCAN or DOCS_SCAN. These phony nodes
# diagnose only the two ready, independent scans. Later stages become plannable only
# after their real prerequisites exist. Fake GCP/storage values plus
# --no-catalogue make these checks local, credential-free, and non-mutating.
define PLAN_STAGE
	@env -i PATH="$(PATH)" \
		DISPATCH_INPUT_ROOT=cloud://example-plan-input \
		DISPATCH_OUTPUT_ROOT=cloud://example-plan-output \
		CLDMUX_GCP_PROJECT=example-project \
		CLDMUX_GCP_REGION=europe-west4 \
		"$(DISPATCH)" \
			--id="$(PIPELINE_ID)-$(1)" \
			--policy=gcp --no-catalogue \
			--image="$(IMAGE)" \
			--input="$(2)" --output="$(PLAN_DIR)/$(1).tar.zst" \
			--cpus="$(CPUS)" --memory-gb="$(MEMORY_GB)" \
			--retries="$(CLOUD_RETRIES)" --timeout="$(CLOUD_TIMEOUT)" \
			--expected-runtime="$(CLOUD_EXPECTED_RUNTIME)" \
			-- /app/pipeline-worker "$(1)" \
			"/dispatch/input/runs/$(PIPELINE_ID)-$(1)/input.tar.zst" \
			"/dispatch/output/runs/$(PIPELINE_ID)-$(1)/output.tar.zst"
endef

cloud-plan: cloud-plan-code cloud-plan-docs
	@printf '%s\n' 'status=planned-first-runnable-frontier'

cloud-plan-code: $(CODE_INPUT) $(DISPATCH) | $(PLAN_DIR)
	$(call PLAN_STAGE,scan-code,$(CODE_INPUT))

cloud-plan-docs: $(DOCS_INPUT) $(DISPATCH) | $(PLAN_DIR)
	$(call PLAN_STAGE,scan-docs,$(DOCS_INPUT))

# CLEANUP CHOICE
#
# There is intentionally no broad clean target. Deleting local cloud receipts
# does not delete provider objects, and blindly rebuilding the same immutable run
# ID can duplicate or conflict with work. Inspect any `.pending` record and its
# run_id before recovery. A caller may remove a specifically reviewed local-only
# result directory with ordinary filesystem tools.

# Repository CI expands both execution adapters without starting Docker,
# contacting a provider, or approving a submission. Runtime correctness of the
# dispatcher itself remains covered by its loopback tests.
check:
	@$(MAKE) --no-print-directory -f "$(MAKEFILE_SELF)" help >/dev/null
	@set -eu; \
		output=$$($(MAKE) --no-print-directory -B -n -f "$(MAKEFILE_SELF)" pipeline \
			BACKEND=local \
			IMAGE=example.test/worker@sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef); \
		printf '%s\n' "$$output" | grep -q -- '--entrypoint=/app/pipeline-worker'
	@set -eu; \
		output=$$($(MAKE) --no-print-directory -B -n -f "$(MAKEFILE_SELF)" pipeline \
			BACKEND=gcp APPROVE_CLOUD=YES \
			IMAGE=example.test/worker@sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef); \
		printf '%s\n' "$$output" | grep -q -- '--submit'; \
		printf '%s\n' "$$output" | grep -q -- 'receipt_status=complete'
	@set -eu; \
		output=$$($(MAKE) --no-print-directory -B -n -f "$(MAKEFILE_SELF)" cloud-plan); \
		printf '%s\n' "$$output" | grep -q -- '--no-catalogue'; \
		! printf '%s\n' "$$output" | grep -q -- '--submit'
