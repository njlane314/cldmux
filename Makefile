CXX ?= c++
CXX_STANDARD ?= c++17
CXXFLAGS ?= -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror
CURL_CXXFLAGS := $(shell curl-config --cflags)
CURL_LIBS := $(shell curl-config --libs)
TST_DIR ?= ../tst
BUILD_DIR ?= build

AMALGAMATOR := $(BUILD_DIR)/amalgamate
AMALGAMATE_BASE_ARGS := --root include/cloud/cloud.hpp --include-root include
AMALGAMATE_ARGS := $(AMALGAMATE_BASE_ARGS) --output single_include/cloud.h
MODULAR_HEADERS := $(shell find include/cloud -type f -name '*.hpp' | sort)
HEADER_PROBES := tests/compile/api.cpp tests/compile/client.cpp \
	tests/compile/job.cpp tests/compile/plan.cpp tests/compile/config.cpp \
	tests/compile/http.cpp tests/compile/json.cpp tests/compile/pricing.cpp \
	tests/compile/provider.cpp tests/compile/storage.cpp \
	tests/compile/submission.cpp tests/compile/gcp.cpp tests/compile/aws.cpp \
	tests/compile/azure.cpp tests/compile/modular.cpp

TEST_MODULAR := /tmp/cloud-modular-test
TEST_AMALGAMATED := /tmp/cloud-amalgamated-test
TEST_SAN_MODULAR := /tmp/cloud-modular-test-san
TEST_SAN_AMALGAMATED := /tmp/cloud-amalgamated-test-san
EXAMPLE_MODULAR := /tmp/cloud-example-modular
EXAMPLE_AMALGAMATED := /tmp/cloud-example-amalgamated
RUN_MODULAR := /tmp/cloud-run-modular
RUN_AMALGAMATED := /tmp/cloud-run-amalgamated
ODR_MODULAR := /tmp/cloud-odr-modular
ODR_AMALGAMATED := /tmp/cloud-odr-amalgamated

.PHONY: check check-readme check-headers check-tool check-modular \
	check-amalgamation check-amalgamated check-odr check-odr-modular \
	check-odr-amalgamated check-cli check-standards check-c++17 check-c++20 \
	check-c++23 amalgamate example example-amalgamated examples \
	examples-amalgamated sanitise

check: check-readme check-headers check-tool check-modular check-amalgamated \
	check-odr check-cli example-amalgamated examples-amalgamated
	@! grep -n "$$(printf '\t')" cloud.h single_include/cloud.h \
		$$(find include tests tools -type f) example.cpp examples/run.cpp \
		examples/support.h test.cpp README.md

check-readme:
	awk '/^```cpp$$/ {code=1; next} code && /^```/ {exit} code {print}' README.md | \
		$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-Isingle_include -x c++ -fsyntax-only -

check-headers:
	@set -e; for header in $(MODULAR_HEADERS); do \
		name=$${header#include/}; \
		printf '#include <%s>\nint main() {}\n' "$$name" | \
			$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
			-Iinclude -x c++ -fsyntax-only -; \
	done
	@set -e; for source in $(HEADER_PROBES); do \
		$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
			-Iinclude -fsyntax-only "$$source"; \
	done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(AMALGAMATOR): tools/amalgamate.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -std=c++17 -O2 $< -o $@

amalgamate: $(AMALGAMATOR)
	$(AMALGAMATOR) $(AMALGAMATE_ARGS) --write
	$(AMALGAMATOR) $(AMALGAMATE_BASE_ARGS) --output cloud.h --write
	cmp -s cloud.h single_include/cloud.h

check-tool: $(AMALGAMATOR)
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" sh tests/amalgamate.sh $(AMALGAMATOR)

check-amalgamation: $(AMALGAMATOR)
	$(AMALGAMATOR) $(AMALGAMATE_ARGS) --check
	$(AMALGAMATOR) $(AMALGAMATE_BASE_ARGS) --output cloud.h --check
	$(AMALGAMATOR) $(AMALGAMATE_ARGS) --stdout | cmp -s - single_include/cloud.h
	cmp -s cloud.h single_include/cloud.h
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-Isingle_include -fsyntax-only tests/compile/amalgamated.cpp

check-modular:
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -Iinclude -I. \
		-isystem "$(TST_DIR)" test.cpp $(CURL_LIBS) -pthread -o $(TEST_MODULAR)
	$(TEST_MODULAR)

check-amalgamated: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-DCLOUD_TEST_AMALGAMATED -Isingle_include -I. -isystem "$(TST_DIR)" \
		test.cpp $(CURL_LIBS) -pthread -o $(TEST_AMALGAMATED)
	$(TEST_AMALGAMATED)

check-odr: check-odr-modular check-odr-amalgamated

check-odr-modular:
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -Iinclude \
		-c tests/compile/odr_a.cpp -o $(ODR_MODULAR)-a.o
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -Iinclude \
		-c tests/compile/odr_b.cpp -o $(ODR_MODULAR)-b.o
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -c tests/compile/odr_main.cpp \
		-o $(ODR_MODULAR)-main.o
	$(CXX) $(ODR_MODULAR)-a.o $(ODR_MODULAR)-b.o $(ODR_MODULAR)-main.o \
		$(CURL_LIBS) -pthread -o $(ODR_MODULAR)
	$(ODR_MODULAR)

check-odr-amalgamated: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-DCLOUD_TEST_AMALGAMATED -Isingle_include \
		-c tests/compile/odr_a.cpp -o $(ODR_AMALGAMATED)-a.o
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-DCLOUD_TEST_AMALGAMATED -Isingle_include \
		-c tests/compile/odr_b.cpp -o $(ODR_AMALGAMATED)-b.o
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -c tests/compile/odr_main.cpp \
		-o $(ODR_AMALGAMATED)-main.o
	$(CXX) $(ODR_AMALGAMATED)-a.o $(ODR_AMALGAMATED)-b.o \
		$(ODR_AMALGAMATED)-main.o $(CURL_LIBS) -pthread -o $(ODR_AMALGAMATED)
	$(ODR_AMALGAMATED)

example:
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -Iinclude -I. \
		example.cpp $(CURL_LIBS) -pthread -o $(EXAMPLE_MODULAR)

example-amalgamated: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-DCLOUD_TEST_AMALGAMATED -Isingle_include -I. example.cpp \
		$(CURL_LIBS) -pthread -o $(EXAMPLE_AMALGAMATED)

examples:
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -Iinclude -I. \
		examples/run.cpp $(CURL_LIBS) -pthread -o $(RUN_MODULAR)

examples-amalgamated: check-amalgamation
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-DCLOUD_TEST_AMALGAMATED -Isingle_include -I. examples/run.cpp \
		$(CURL_LIBS) -pthread -o $(RUN_AMALGAMATED)

check-cli: example examples
	@output="$$(env -i PATH="$(PATH)" CLOUD_GCP_PROJECT=test-project \
		CLOUD_GCP_REGION=europe-west4 $(RUN_MODULAR) gcp)"; \
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
		printf '%s\n' "$$output" | grep -q '^program=cloud-run$$'; \
		printf '%s\n' "$$output" | grep -q '^status=dry-run$$'; \
		! printf '%s\n' "$$output" | grep -Ev '^[a-z_]+=.*$$'
	@output="$$(env -i PATH="$(PATH)" CLOUD_GCP_PROJECT=test-project \
		CLOUD_GCP_REGION=europe-west4 $(EXAMPLE_MODULAR) gcp)"; \
		printf '%s\n' "$$output" | grep -q '^application=simulation$$'; \
		printf '%s\n' "$$output" | grep -q '^preflight=planned$$'; \
		printf '%s\n' "$$output" | grep -q '^status=dry-run$$'
	@output="$$(env -i PATH="$(PATH)" CLOUD_AWS_JOB_QUEUE=test-queue \
		CLOUD_AWS_REGION=eu-west-1 $(RUN_MODULAR) aws)"; \
		printf '%s\n' "$$output" | grep -q '^provider=aws$$'; \
		printf '%s\n' "$$output" | grep -q '^provider_job_timeout_seconds=not-applicable$$'
	@output="$$(env -i PATH="$(PATH)" \
		CLOUD_AZURE_BATCH_ENDPOINT=https://test.westeurope.batch.azure.com \
		CLOUD_AZURE_REGION=westeurope $(RUN_MODULAR) azure)"; \
		printf '%s\n' "$$output" | grep -q '^provider=azure$$'; \
		printf '%s\n' "$$output" | grep -q '^provider_job_timeout_seconds=1290$$'
	@status=0; env -i PATH="$(PATH)" $(RUN_MODULAR) >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(RUN_MODULAR) gcp extra >/dev/null 2>&1 || status=$$?; test $$status -eq 2
	@status=0; bad="$$(printf 'bad\nprovider')"; \
		output="$$( $(RUN_MODULAR) "$$bad" 2>&1)" || status=$$?; \
		test $$status -eq 2; \
		test "$$(printf '%s\n' "$$output" | wc -l | tr -d ' ')" -eq 1; \
		printf '%s\n' "$$output" | grep -Fq '\n'
	@status=0; $(RUN_MODULAR) gcp --submit --submit >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(RUN_MODULAR) gcp --estimate --estimate >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(RUN_MODULAR) gcp --expected-attempt-runtime=0s >/dev/null 2>&1 || \
		status=$$?; test $$status -eq 2
	@status=0; $(RUN_MODULAR) gcp --expected-attempt-runtime=15 >/dev/null 2>&1 || \
		status=$$?; test $$status -eq 2
	@status=0; output="$$(env -i PATH="$(PATH)" CLOUD_GCP_PROJECT=test-project \
		CLOUD_GCP_REGION=europe-west4 $(RUN_MODULAR) gcp \
		--expected-attempt-runtime=16m 2>&1)" || status=$$?; \
		test $$status -eq 2; \
		test "$$output" = 'error=Expected attempt runtime must not exceed the controller timeout'
	@status=0; $(RUN_MODULAR) gcp --unknown >/dev/null 2>&1 || status=$$?; \
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
		-fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude -I. \
		-isystem "$(TST_DIR)" test.cpp $(CURL_LIBS) -pthread -o $(TEST_SAN_MODULAR)
	$(TEST_SAN_MODULAR)
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-DCLOUD_TEST_AMALGAMATED -Isingle_include -I. -isystem "$(TST_DIR)" \
		test.cpp $(CURL_LIBS) -pthread -o $(TEST_SAN_AMALGAMATED)
	$(TEST_SAN_AMALGAMATED)
