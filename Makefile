CXX ?= c++
CXX_STANDARD ?= c++17
CXX26_STANDARD ?=
CXXFLAGS ?= -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror
CURL_FLAGS := $(shell curl-config --cflags --libs)
TST_DIR ?= ../tst
TEST := /tmp/cloud-h-test
EXAMPLE := /tmp/cloud-h-example
RUN_EXAMPLE := /tmp/cloud-h-run

.PHONY: check check-standards check-c++17 check-c++20 check-c++23 check-c++26 \
	example examples sanitize
check: example examples
	awk '/^```cpp$$/ {code=1; next} code && /^```/ {exit} code {print}' README.md | \
		$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -I. -x c++ -fsyntax-only -
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -I. -isystem "$(TST_DIR)" \
		test.cpp $(CURL_FLAGS) -pthread -o $(TEST)
	$(TEST)
	@output="$$(env -i PATH="$(PATH)" CLOUD_GCP_PROJECT=test-project \
		CLOUD_GCP_REGION=europe-west4 $(RUN_EXAMPLE) gcp)"; \
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
		printf '%s\n' "$$output" | grep -q '^status=dry-run$$'; \
		! printf '%s\n' "$$output" | grep -Ev '^[a-z_]+=.*$$'
	@output="$$(env -i PATH="$(PATH)" CLOUD_AWS_JOB_QUEUE=test-queue \
		CLOUD_AWS_REGION=eu-west-1 $(RUN_EXAMPLE) aws)"; \
		printf '%s\n' "$$output" | grep -q '^provider=aws$$'; \
		printf '%s\n' "$$output" | grep -q '^provider_job_timeout_seconds=not-applicable$$'
	@output="$$(env -i PATH="$(PATH)" \
		CLOUD_AZURE_BATCH_ENDPOINT=https://test.westeurope.batch.azure.com \
		CLOUD_AZURE_REGION=westeurope $(RUN_EXAMPLE) azure)"; \
		printf '%s\n' "$$output" | grep -q '^provider=azure$$'; \
		printf '%s\n' "$$output" | grep -q '^provider_job_timeout_seconds=1290$$'
	@status=0; env -i PATH="$(PATH)" $(RUN_EXAMPLE) >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(RUN_EXAMPLE) gcp extra >/dev/null 2>&1 || status=$$?; test $$status -eq 2
	@status=0; bad="$$(printf 'bad\nprovider')"; \
		output="$$( $(RUN_EXAMPLE) "$$bad" 2>&1)" || status=$$?; \
		test $$status -eq 2; \
		test "$$(printf '%s\n' "$$output" | wc -l | tr -d ' ')" -eq 1; \
		printf '%s\n' "$$output" | grep -Fq '\n'
	@status=0; $(RUN_EXAMPLE) gcp --submit --submit >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(RUN_EXAMPLE) gcp --estimate --estimate >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(RUN_EXAMPLE) gcp --expected-attempt-runtime=0s >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(RUN_EXAMPLE) gcp --expected-attempt-runtime=15 >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; output="$$(env -i PATH="$(PATH)" CLOUD_GCP_PROJECT=test-project \
		CLOUD_GCP_REGION=europe-west4 $(RUN_EXAMPLE) gcp \
		--expected-attempt-runtime=16m 2>&1)" || status=$$?; \
		test $$status -eq 2; \
		test "$$output" = 'error=Expected attempt runtime must not exceed the controller timeout'
	@status=0; $(RUN_EXAMPLE) gcp --unknown >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@! grep -n "$$(printf '\t')" cloud.h example.cpp examples/run.cpp examples/support.h test.cpp \
		README.md

check-standards:
	$(MAKE) check-c++17
	$(MAKE) check-c++20
	$(MAKE) check-c++23
	$(MAKE) check-c++26

check-c++17:
	$(MAKE) check CXX_STANDARD=c++17

check-c++20:
	$(MAKE) check CXX_STANDARD=c++20

check-c++23:
	$(MAKE) check CXX_STANDARD=c++23

# Compiler flag spellings are still transitional. Try the final spelling first,
# then the widely supported draft spelling, unless the caller chooses one.
check-c++26:
	@standard="$(CXX26_STANDARD)"; \
	if test -n "$$standard"; then \
		candidates="$$standard"; \
	else \
		candidates="c++26 c++2c"; \
	fi; \
	for candidate in $$candidates; do \
		if printf '%s\n' 'int main() {}' | \
			$(CXX) -std=$$candidate -x c++ -fsyntax-only - >/dev/null 2>&1; then \
			standard="$$candidate"; \
			break; \
		fi; \
	done; \
	if test -z "$$standard" || ! printf '%s\n' 'int main() {}' | \
		$(CXX) -std=$$standard -x c++ -fsyntax-only - >/dev/null 2>&1; then \
		if test -n "$(CXX26_STANDARD)"; then \
			echo "C++26 check skipped: $(CXX) does not support -std=$(CXX26_STANDARD)"; \
		else \
			echo "C++26 check skipped: $(CXX) supports neither -std=c++26 nor -std=c++2c"; \
		fi; \
		exit 0; \
	fi; \
	echo "Checking C++26 compatibility with -std=$$standard"; \
	$(MAKE) check CXX_STANDARD=$$standard

example:
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -I. example.cpp \
		$(CURL_FLAGS) -pthread -o $(EXAMPLE)

examples:
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -I. examples/run.cpp \
		$(CURL_FLAGS) -pthread -o $(RUN_EXAMPLE)

sanitize:
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -fsanitize=address,undefined \
		-fno-omit-frame-pointer \
		-I. -isystem "$(TST_DIR)" test.cpp $(CURL_FLAGS) -pthread -o $(TEST)-san
	$(TEST)-san
