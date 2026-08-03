CXX ?= c++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror
CURL_FLAGS := $(shell curl-config --cflags --libs)
TST_DIR ?= ../tst
TEST := /tmp/cloud-h-test
EXAMPLE := /tmp/cloud-h-example
RUN_EXAMPLE := /tmp/cloud-h-run

.PHONY: check example examples sanitize
check: example examples $(TST_DIR)/tst.hpp
	awk '/^```cpp$$/ {code=1; next} code && /^```/ {exit} code {print}' README.md | \
		$(CXX) $(CXXFLAGS) -I. -x c++ -fsyntax-only -
	$(CXX) $(CXXFLAGS) -I. -isystem "$(TST_DIR)" test.cpp $(CURL_FLAGS) -pthread -o $(TEST)
	$(TEST)
	env -i PATH="$(PATH)" CLOUD_GCP_PROJECT=test-project CLOUD_GCP_REGION=europe-west4 \
		$(RUN_EXAMPLE) gcp
	@env -i PATH="$(PATH)" CLOUD_AWS_JOB_QUEUE=test-queue CLOUD_AWS_REGION=eu-west-1 \
		$(RUN_EXAMPLE) aws >/dev/null
	@env -i PATH="$(PATH)" CLOUD_AZURE_BATCH_ENDPOINT=https://test.westeurope.batch.azure.com \
		CLOUD_AZURE_REGION=westeurope \
		$(RUN_EXAMPLE) azure >/dev/null
	@status=0; env -i PATH="$(PATH)" $(RUN_EXAMPLE) >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2
	@status=0; $(RUN_EXAMPLE) gcp extra >/dev/null 2>&1 || status=$$?; test $$status -eq 2
	@status=0; $(RUN_EXAMPLE) gcp --submit --submit >/dev/null 2>&1 || status=$$?; \
		test $$status -eq 2

example:
	$(CXX) $(CXXFLAGS) -I. example.cpp $(CURL_FLAGS) -pthread -o $(EXAMPLE)

examples:
	$(CXX) $(CXXFLAGS) -I. examples/run.cpp $(CURL_FLAGS) -pthread -o $(RUN_EXAMPLE)

sanitize: $(TST_DIR)/tst.hpp
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I. -isystem "$(TST_DIR)" test.cpp $(CURL_FLAGS) -pthread -o $(TEST)-san
	$(TEST)-san
