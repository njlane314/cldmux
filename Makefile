CXX ?= c++
CXX_STANDARD ?= c++17
CXXFLAGS ?= -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror
CURL_CXXFLAGS := $(shell curl-config --cflags)
CURL_LIBS := $(shell curl-config --libs)
TST_DIR ?= ../tst
BUILD_DIR ?= build

AMALGAMATOR := $(BUILD_DIR)/amalgamate
AMALGAMATE_ARGS := --root include/cldmux/cldmux.hpp --include-root include --output cldmux
INTERNAL_HEADERS := $(shell find include/cldmux -type f -name '*.hpp' | sort)
INTERNAL_HEADER_PROBES := tests/compile/api.cpp tests/compile/client.cpp \
    tests/compile/router.cpp \
    tests/compile/job.cpp tests/compile/plan.cpp tests/compile/config.cpp \
    tests/compile/http.cpp tests/compile/json.cpp tests/compile/pricing.cpp \
    tests/compile/provider.cpp tests/compile/storage.cpp \
    tests/compile/submission.cpp tests/compile/gcp.cpp tests/compile/aws.cpp \
    tests/compile/azure.cpp

TEST := /tmp/cldmux-test
TEST_SAN := /tmp/cldmux-test-san
EXAMPLE := /tmp/cldmux-example
EMPIRICAL := /tmp/cldmux-empirical
EMPIRICAL_SAN := /tmp/cldmux-empirical-san
DISPATCH := /tmp/cldmux-dispatch
DISPATCH_TEST := /tmp/cldmux-dispatch-test
DISPATCH_SAN := /tmp/cldmux-dispatch-san
DISPATCH_TEST_SAN := /tmp/cldmux-dispatch-test-san
ODR := /tmp/cldmux-odr

.PHONY: check check-readme check-headers check-tool check-amalgamation \
    check-library check-odr check-cli check-empirical check-dispatch \
    check-example-make \
    check-dispatch-header check-stale-names check-standards check-c++17 \
    check-c++20 check-c++23 amalgamate example empirical dispatch sanitise

check: check-readme check-headers check-tool check-library check-odr check-cli \
    check-empirical check-dispatch check-example-make check-stale-names
	@! grep -n "$$(printf '\t')" cldmux \
		$$(find apps include tests tools -type f) example.cpp test.cpp README.md

check-stale-names:
	@set -eu; \
		names=$$(find . -path './.git' -prune -o -path './build' -prune -o \
			-iname '*bu[r]st*' -print); \
		test -z "$$names"; \
		old_paths=$$(find . -path './.git' -prune -o -path './build' -prune -o \
			\( -name 'cl[o]ud' -o -path './include/cl[o]ud/*' \) -print); \
		test -z "$$old_paths"; \
		status=0; \
		grep -Rni 'bu[r]st' apps include tests tools .github || status=$$?; \
		test "$$status" -eq 1; \
		status=0; \
		grep -ni 'bu[r]st' Makefile example.mk README.md cldmux example.cpp test.cpp || \
			status=$$?; \
		test "$$status" -eq 1; \
		status=0; \
		git grep -nE '#include[[:space:]]*[<"]cl[o]ud([/>"])|cl[o]ud::|'\
'namespace[[:space:]]+cl[o]ud|CLO[U]D_H_VERSION|NJLANE314_CLO[U]D|'\
'(^|[^A-Z0-9_])CLO[U]D_(REGION|ZONE|GCP_|AWS_|AZURE_|COMPUTE_TEMPLATE)|'\
'cl[o]ud-(run|empirical|example|dispatch|test|odr|amalgamate)|'\
'\.cl[o]ud-empirical-history|njlane314/cl[o]ud' -- . || status=$$?; \
		test "$$status" -eq 1

check-readme:
	awk '/^```cpp$$/ {code=1; next} code && /^```/ {exit} code {print}' README.md | \
		$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-I. -x c++ -fsyntax-only -

check-example-make:
	$(MAKE) -f example.mk check

check-headers:
	@set -e; for header in $(INTERNAL_HEADERS); do \
		name=$${header#include/}; \
		printf '#include <%s>\nint main() {}\n' "$$name" | \
			$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
			-Iinclude -x c++ -fsyntax-only -; \
	done
	@set -e; for source in $(INTERNAL_HEADER_PROBES); do \
		$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
			-Iinclude -fsyntax-only "$$source"; \
	done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(AMALGAMATOR): tools/amalgamate.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -std=c++17 -O2 $< -o $@

amalgamate: $(AMALGAMATOR)
	$(AMALGAMATOR) $(AMALGAMATE_ARGS) --write

check-tool: $(AMALGAMATOR)
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" sh tests/amalgamate.sh $(AMALGAMATOR)

check-amalgamation: $(AMALGAMATOR)
	$(AMALGAMATOR) $(AMALGAMATE_ARGS) --check
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-fsyntax-only tests/compile/public.cpp

check-library: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-isystem "$(TST_DIR)" test.cpp $(CURL_LIBS) -pthread -o $(TEST)
	$(TEST)

check-odr: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-c tests/compile/odr_a.cpp -o $(ODR)-a.o
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-c tests/compile/odr_b.cpp -o $(ODR)-b.o
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -c tests/compile/odr_main.cpp \
		-o $(ODR)-main.o
	$(CXX) $(ODR)-a.o $(ODR)-b.o $(ODR)-main.o $(CURL_LIBS) -pthread -o $(ODR)
	$(ODR)

example: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		example.cpp $(CURL_LIBS) -pthread -o $(EXAMPLE)

empirical: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		apps/empirical.cpp $(CURL_LIBS) -pthread -o $(EMPIRICAL)

check-empirical: empirical
	sh tests/empirical.sh $(EMPIRICAL)

dispatch: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		apps/dispatch.cpp apps/dispatch_main.cpp $(CURL_LIBS) -pthread -o $(DISPATCH)

check-dispatch-header:
	@! grep -nE '^[[:space:]]*#include[[:space:]].*cldmux|cldmux::' apps/dispatch.hpp
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-fsyntax-only tests/compile/dispatch.cpp

check-dispatch: check-dispatch-header dispatch
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -DDISPATCH_TESTING \
		-I. apps/dispatch.cpp tests/dispatch.cpp $(CURL_LIBS) -pthread \
		-o $(DISPATCH_TEST)
	$(DISPATCH_TEST)
	sh tests/dispatch.sh $(DISPATCH)

check-cli: example
	@output="$$(env -i PATH="$(PATH)" CLDMUX_GCP_PROJECT=test-project \
		CLDMUX_GCP_REGION=europe-west4 $(EXAMPLE) gcp)"; \
		printf '%s\n' "$$output"; \
		printf '%s\n' "$$output" | grep -q '^output_version=1$$'; \
		printf '%s\n' "$$output" | grep -q '^requested_provider=gcp$$'; \
		printf '%s\n' "$$output" | grep -q '^provider=gcp$$'; \
		printf '%s\n' "$$output" | grep -q '^region=europe-west4$$'; \
		printf '%s\n' "$$output" | grep -q '^machine=e2-standard-4$$'; \
		printf '%s\n' "$$output" | grep -q '^expected_attempt_runtime_seconds=300$$'; \
		printf '%s\n' "$$output" | grep -q '^controller_timeout_seconds=900$$'; \
		printf '%s\n' "$$output" | grep -q '^provider_attempt_timeout_seconds=900$$'; \
		printf '%s\n' "$$output" | grep -q '^configured_attempt_limit=2$$'; \
		printf '%s\n' "$$output" | grep -q '^hourly_rate_estimate_usd=unavailable$$'; \
		printf '%s\n' "$$output" | \
			grep -q '^estimated_cost_for_expected_attempt_runtime_usd=unavailable$$'; \
		printf '%s\n' "$$output" | grep -q '^preflight=planned$$'; \
		printf '%s\n' "$$output" | grep -q '^program=cldmux-run$$'; \
		printf '%s\n' "$$output" | grep -q '^status=dry-run$$'; \
		! printf '%s\n' "$$output" | grep -Ev '^[a-z_]+=.*$$'
	@output="$$(env -i PATH="$(PATH)" CLDMUX_AWS_JOB_QUEUE=test-queue \
		CLDMUX_AWS_REGION=eu-west-1 $(EXAMPLE) aws)"; \
		printf '%s\n' "$$output" | grep -q '^provider=aws$$'; \
		printf '%s\n' "$$output" | grep -q '^provider_job_timeout_seconds=not-applicable$$'
	@output="$$(env -i PATH="$(PATH)" \
		CLDMUX_AZURE_BATCH_ENDPOINT=https://test.westeurope.batch.azure.com \
		CLDMUX_AZURE_REGION=westeurope $(EXAMPLE) azure)"; \
		printf '%s\n' "$$output" | grep -q '^provider=azure$$'; \
		printf '%s\n' "$$output" | grep -q '^provider_job_timeout_seconds=1290$$'
	@output="$$($(EXAMPLE) --help)"; \
		printf '%s\n' "$$output" | grep -q '^Usage: cldmux-run '; \
		printf '%s\n' "$$output" | grep -q '^  cheapest   compare every configured provider'
	@status=0; env -i PATH="$(PATH)" $(EXAMPLE) >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(EXAMPLE) gcp extra >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; bad="$$(printf 'bad\nprovider')"; \
		output="$$( $(EXAMPLE) "$$bad" 2>&1)" || status=$$?; \
		test $$status -eq 2; \
		test "$$(printf '%s\n' "$$output" | wc -l | tr -d ' ')" -eq 1; \
		printf '%s\n' "$$output" | grep -Fq '\n'
	@status=0; $(EXAMPLE) gcp --submit --submit >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(EXAMPLE) gcp --estimate --estimate >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(EXAMPLE) gcp --help --help >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@set -e; for value in 0s -1s 15 1d 18446744073709551615h; do \
		status=0; $(EXAMPLE) gcp \
			--expected-attempt-runtime=$$value >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2; \
	done
	@status=0; $(EXAMPLE) gcp --expected-attempt-runtime=1m \
		--expected-attempt-runtime=2m >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@output="$$(env -i PATH="$(PATH)" CLDMUX_GCP_PROJECT=test-project \
		CLDMUX_GCP_REGION=europe-west4 $(EXAMPLE) gcp \
		--expected-attempt-runtime=30s)"; \
		printf '%s\n' "$$output" | grep -q '^expected_attempt_runtime_seconds=30$$'
	@status=0; output="$$(env -i PATH="$(PATH)" CLDMUX_GCP_PROJECT=test-project \
		CLDMUX_GCP_REGION=europe-west4 $(EXAMPLE) gcp \
		--expected-attempt-runtime=16m 2>&1)" || status=$$?; \
		test $$status -eq 2; \
		test "$$output" = 'error=Expected attempt runtime must not exceed the controller timeout'
	@status=0; $(EXAMPLE) gcp --unknown >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2

check-standards:
	$(MAKE) check-c++17
	$(MAKE) check-c++20
	$(MAKE) check-c++23

check-c++17:
	$(MAKE) check CXX_STANDARD=c++17

check-c++20:
	$(MAKE) check CXX_STANDARD=c++20

check-c++23:
	$(MAKE) check CXX_STANDARD=c++23

sanitise: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-fsanitize=address,undefined -fno-omit-frame-pointer -I. \
		-isystem "$(TST_DIR)" test.cpp $(CURL_LIBS) -pthread -o $(TEST_SAN)
	$(TEST_SAN)
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-fsanitize=address,undefined -fno-omit-frame-pointer -I. \
		apps/empirical.cpp $(CURL_LIBS) -pthread -o $(EMPIRICAL_SAN)
	sh tests/empirical.sh $(EMPIRICAL_SAN)
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-fsanitize=address,undefined -fno-omit-frame-pointer -I. \
		apps/dispatch.cpp apps/dispatch_main.cpp $(CURL_LIBS) -pthread \
		-o $(DISPATCH_SAN)
	sh tests/dispatch.sh $(DISPATCH_SAN)
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -DDISPATCH_TESTING \
		-fsanitize=address,undefined -fno-omit-frame-pointer -I. \
		apps/dispatch.cpp tests/dispatch.cpp $(CURL_LIBS) -pthread \
		-o $(DISPATCH_TEST_SAN)
	$(DISPATCH_TEST_SAN)
