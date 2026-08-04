CXX ?= c++
CXX_STANDARD ?= c++17
CXXFLAGS ?= -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror
CURL_CONFIG ?= curl-config
CURL_CXXFLAGS ?= $(shell $(CURL_CONFIG) --cflags)
CURL_LIBS ?= $(shell $(CURL_CONFIG) --libs)
TST_DIR ?= ../tst
BUILD_DIR ?= build
CHECK_DIR ?= $(BUILD_DIR)/check
BINARY_DIR ?= $(BUILD_DIR)/bin
RELEASE_DIR ?= $(BUILD_DIR)/release
BINARY_CXXFLAGS ?= -O2 -DNDEBUG -fvisibility=hidden -fvisibility-inlines-hidden

MACOS_STRIP ?= strip
MACOS_STRIP_FLAGS ?= -S -x
LINUX_STRIP ?=
LINUX_STRIP_FLAGS ?= --strip-all
WINDOWS_STRIP ?=
WINDOWS_STRIP_FLAGS ?= --strip-all

MACOS_MIN_VERSION ?= 13.0
MACOS_SIGN_IDENTITY ?=
MACOS_INSTALLER_IDENTITY ?=
MACOS_NOTARY_PROFILE ?=
RELEASE_VERSION ?=

HOST_SYSTEM := $(shell uname -s 2>/dev/null)
ifeq ($(OS),Windows_NT)
HOST_PLATFORM := windows
HOST_EXEEXT := .exe
else ifeq ($(HOST_SYSTEM),Darwin)
HOST_PLATFORM := macos
HOST_EXEEXT :=
else ifeq ($(HOST_SYSTEM),Linux)
HOST_PLATFORM := linux
HOST_EXEEXT :=
else
HOST_PLATFORM := unknown
HOST_EXEEXT :=
endif

MACOS_TARGET_FLAGS ?=
MACOS_THREAD_FLAGS ?= -pthread
LINUX_TARGET_FLAGS ?=
LINUX_THREAD_FLAGS ?= -pthread
WINDOWS_TARGET_FLAGS ?=
WINDOWS_THREAD_FLAGS ?= -pthread

ifeq ($(HOST_PLATFORM),macos)
MACOS_CXX ?= $(CXX)
MACOS_CURL_CXXFLAGS ?= $(CURL_CXXFLAGS)
MACOS_CURL_LIBS ?= $(CURL_LIBS)
else
MACOS_CXX ?= clang++
MACOS_CURL_CXXFLAGS ?=
MACOS_CURL_LIBS ?=
endif

ifeq ($(HOST_PLATFORM),linux)
LINUX_CXX ?= $(CXX)
LINUX_CURL_CXXFLAGS ?= $(CURL_CXXFLAGS)
LINUX_CURL_LIBS ?= $(CURL_LIBS)
else
LINUX_CXX ?= x86_64-linux-gnu-g++
LINUX_CURL_CXXFLAGS ?=
LINUX_CURL_LIBS ?=
endif

ifeq ($(HOST_PLATFORM),windows)
WINDOWS_CXX ?= $(CXX)
WINDOWS_CURL_CXXFLAGS ?= $(CURL_CXXFLAGS)
WINDOWS_CURL_LIBS ?= $(CURL_LIBS)
else
WINDOWS_CXX ?= x86_64-w64-mingw32-g++
WINDOWS_CURL_CXXFLAGS ?=
WINDOWS_CURL_LIBS ?=
endif

MACOS_DISPATCH := $(BINARY_DIR)/macos/cldmux-dispatch
LINUX_DISPATCH := $(BINARY_DIR)/linux/cldmux-dispatch
WINDOWS_DISPATCH := $(BINARY_DIR)/windows/cldmux-dispatch.exe

AMALGAMATOR := $(BUILD_DIR)/tools/$(HOST_PLATFORM)/amalgamate$(HOST_EXEEXT)
AMALGAMATE_ARGS := --root include/cldmux/cldmux.hpp --include-root include --output cldmux
INTERNAL_HEADERS := $(shell find include/cldmux -type f -name '*.hpp' | sort)
INTERNAL_HEADER_PROBES := tests/compile/api.cpp tests/compile/client.cpp \
    tests/compile/router.cpp \
    tests/compile/job.cpp tests/compile/plan.cpp tests/compile/config.cpp \
    tests/compile/http.cpp tests/compile/json.cpp tests/compile/pricing.cpp \
    tests/compile/provider.cpp tests/compile/storage.cpp \
    tests/compile/submission.cpp tests/compile/gcp.cpp tests/compile/aws.cpp \
    tests/compile/azure.cpp

TEST := $(CHECK_DIR)/cldmux-test$(HOST_EXEEXT)
TEST_SAN := $(CHECK_DIR)/cldmux-test-san$(HOST_EXEEXT)
EXAMPLE := $(CHECK_DIR)/cldmux-example$(HOST_EXEEXT)
DISPATCH := $(CHECK_DIR)/cldmux-dispatch$(HOST_EXEEXT)
DISPATCH_TEST := $(CHECK_DIR)/cldmux-dispatch-test$(HOST_EXEEXT)
DISPATCH_SAN := $(CHECK_DIR)/cldmux-dispatch-san$(HOST_EXEEXT)
DISPATCH_TEST_SAN := $(CHECK_DIR)/cldmux-dispatch-test-san$(HOST_EXEEXT)
ODR_BASE := $(CHECK_DIR)/cldmux-odr
ODR := $(ODR_BASE)$(HOST_EXEEXT)

.PHONY: check check-readme check-headers check-tool check-amalgamation \
    check-amalgamated-bytes check-library check-odr check-cli check-dispatch \
    check-example-make check-build-layout check-release-macos prepare-build \
    check-dispatch-header check-stale-names check-standards check-c++17 \
    check-c++20 check-c++23 amalgamate example dispatch dispatch-native \
    dispatch-macos dispatch-linux dispatch-windows dispatch-binaries \
    release-macos sanitise

check: check-readme check-headers check-tool check-library check-odr check-cli \
    check-dispatch check-example-make check-stale-names check-build-layout
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
'cl[o]ud-(run|example|dispatch|test|odr|amalgamate)|'\
'njlane314/cl[o]ud' -- . || status=$$?; \
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

prepare-build:
	@bash scripts/prepare-build.sh "$(HOST_PLATFORM)" "$(BUILD_DIR)" "$(CHECK_DIR)"

check-build-layout: | prepare-build
	@! grep -nE '/(private/)?tmp/cldmux-' Makefile README.md example.mk \
		scripts/*.sh .github/workflows/*.yml

$(AMALGAMATOR): tools/amalgamate.cpp | prepare-build
	mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -std=c++17 -O2 $< -o $@

amalgamate: $(AMALGAMATOR)
	$(AMALGAMATOR) $(AMALGAMATE_ARGS) --write

check-tool: $(AMALGAMATOR)
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" sh tests/amalgamate.sh $(AMALGAMATOR)

check-amalgamated-bytes: $(AMALGAMATOR)
	$(AMALGAMATOR) $(AMALGAMATE_ARGS) --check

check-amalgamation: check-amalgamated-bytes
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-fsyntax-only tests/compile/public.cpp

check-library: check-amalgamation | prepare-build
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-isystem "$(TST_DIR)" test.cpp $(CURL_LIBS) -pthread -o $(TEST)
	$(TEST)

check-odr: check-amalgamation | prepare-build
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-c tests/compile/odr_a.cpp -o $(ODR_BASE)-a.o
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-c tests/compile/odr_b.cpp -o $(ODR_BASE)-b.o
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -c tests/compile/odr_main.cpp \
		-o $(ODR_BASE)-main.o
	$(CXX) $(ODR_BASE)-a.o $(ODR_BASE)-b.o $(ODR_BASE)-main.o \
		$(CURL_LIBS) -pthread -o $(ODR)
	$(ODR)

example: check-amalgamation | prepare-build
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		example.cpp $(CURL_LIBS) -pthread -o $(EXAMPLE)

dispatch: check-amalgamation | prepare-build
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		apps/dispatch.cpp $(CURL_LIBS) -pthread -o $(DISPATCH)

ifneq ($(filter macos linux windows,$(HOST_PLATFORM)),)
dispatch-native: dispatch-$(HOST_PLATFORM)
else
dispatch-native:
	@printf '%s\n' 'error: unsupported native platform: $(HOST_SYSTEM)' >&2
	@exit 2
endif

dispatch-binaries: dispatch-macos dispatch-linux dispatch-windows

dispatch-macos: check-amalgamated-bytes apps/dispatch.cpp apps/dispatch.hpp cldmux | prepare-build
	@command -v "$(firstword $(MACOS_CXX))" >/dev/null 2>&1 || { \
		printf '%s\n' 'error: macOS compiler not found: $(firstword $(MACOS_CXX))' >&2; \
		exit 2; \
	}
	@target="$$( $(MACOS_CXX) $(MACOS_TARGET_FLAGS) -dumpmachine 2>/dev/null)" || { \
		printf '%s\n' 'error: cannot query the macOS compiler target' >&2; exit 2; \
	}; \
	case "$$target" in \
		*-apple-darwin*) ;; \
		*) printf '%s\n' "error: macOS compiler targets $$target" >&2; exit 2 ;; \
	esac
	@printf '%s\n' '#if !defined(__APPLE__) || !defined(__MACH__)' \
		'#error compiler does not target macOS' '#endif' | \
		$(MACOS_CXX) $(MACOS_TARGET_FLAGS) -x c++ -fsyntax-only -
	@test -n "$(strip $(MACOS_CURL_LIBS))" || { \
		printf '%s\n' 'error: set MACOS_CURL_CXXFLAGS and MACOS_CURL_LIBS for the target sysroot' >&2; \
		exit 2; \
	}
	@umask 077; mkdir -p "$(dir $(MACOS_DISPATCH))"
	$(MACOS_CXX) $(CXXFLAGS) $(BINARY_CXXFLAGS) $(MACOS_TARGET_FLAGS) \
		$(MACOS_CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. apps/dispatch.cpp \
		$(MACOS_CURL_LIBS) $(MACOS_THREAD_FLAGS) -o "$(MACOS_DISPATCH)"
	$(MACOS_STRIP) $(MACOS_STRIP_FLAGS) "$(MACOS_DISPATCH)"
	@chmod 700 "$(MACOS_DISPATCH)"
	@if test "$(HOST_PLATFORM)" = macos; then \
		codesign --force --sign - --options runtime "$(MACOS_DISPATCH)"; \
		codesign --verify --strict --all-architectures "$(MACOS_DISPATCH)"; \
	fi

dispatch-linux: check-amalgamated-bytes apps/dispatch.cpp apps/dispatch.hpp cldmux | prepare-build
	@command -v "$(firstword $(LINUX_CXX))" >/dev/null 2>&1 || { \
		printf '%s\n' 'error: Linux compiler not found: $(firstword $(LINUX_CXX))' >&2; \
		exit 2; \
	}
	@target="$$( $(LINUX_CXX) $(LINUX_TARGET_FLAGS) -dumpmachine 2>/dev/null)" || { \
		printf '%s\n' 'error: cannot query the Linux compiler target' >&2; exit 2; \
	}; \
	case "$$target" in \
		*linux*) ;; \
		*) printf '%s\n' "error: Linux compiler targets $$target" >&2; exit 2 ;; \
	esac
	@printf '%s\n' '#if !defined(__linux__)' '#error compiler does not target Linux' '#endif' | \
		$(LINUX_CXX) $(LINUX_TARGET_FLAGS) -x c++ -fsyntax-only -
	@test -n "$(strip $(LINUX_CURL_LIBS))" || { \
		printf '%s\n' 'error: set LINUX_CURL_CXXFLAGS and LINUX_CURL_LIBS for the target sysroot' >&2; \
		exit 2; \
	}
	@umask 077; mkdir -p "$(dir $(LINUX_DISPATCH))"
	$(LINUX_CXX) $(CXXFLAGS) $(BINARY_CXXFLAGS) $(LINUX_TARGET_FLAGS) \
		$(LINUX_CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. apps/dispatch.cpp \
		$(LINUX_CURL_LIBS) $(LINUX_THREAD_FLAGS) -o "$(LINUX_DISPATCH)"
	@strip_tool="$(LINUX_STRIP)"; \
		test -n "$$strip_tool" || \
			strip_tool="$$( $(LINUX_CXX) $(LINUX_TARGET_FLAGS) -print-prog-name=strip)"; \
		"$$strip_tool" $(LINUX_STRIP_FLAGS) "$(LINUX_DISPATCH)"
	@chmod 700 "$(LINUX_DISPATCH)"

dispatch-windows: check-amalgamated-bytes apps/dispatch.cpp apps/dispatch.hpp cldmux | prepare-build
	@command -v "$(firstword $(WINDOWS_CXX))" >/dev/null 2>&1 || { \
		printf '%s\n' 'error: Windows compiler not found: $(firstword $(WINDOWS_CXX))' >&2; \
		exit 2; \
	}
	@target="$$( $(WINDOWS_CXX) $(WINDOWS_TARGET_FLAGS) -dumpmachine 2>/dev/null)" || { \
		printf '%s\n' 'error: cannot query the Windows compiler target' >&2; exit 2; \
	}; \
	case "$$target" in \
		*mingw*|*-windows-gnu*) ;; \
		*) printf '%s\n' "error: Windows compiler targets $$target" >&2; exit 2 ;; \
	esac
	@printf '%s\n' '#if !defined(_WIN32)' '#error compiler does not target Windows' '#endif' | \
		$(WINDOWS_CXX) $(WINDOWS_TARGET_FLAGS) -x c++ -fsyntax-only -
	@test -n "$(strip $(WINDOWS_CURL_LIBS))" || { \
		printf '%s\n' 'error: set WINDOWS_CURL_CXXFLAGS and WINDOWS_CURL_LIBS for the target sysroot' >&2; \
		exit 2; \
	}
	@umask 077; mkdir -p "$(dir $(WINDOWS_DISPATCH))"
	$(WINDOWS_CXX) $(CXXFLAGS) $(BINARY_CXXFLAGS) $(WINDOWS_TARGET_FLAGS) \
		$(WINDOWS_CURL_CXXFLAGS) -std=$(CXX_STANDARD) -I. apps/dispatch.cpp \
		$(WINDOWS_CURL_LIBS) $(WINDOWS_THREAD_FLAGS) -o "$(WINDOWS_DISPATCH)"
	@strip_tool="$(WINDOWS_STRIP)"; \
		test -n "$$strip_tool" || \
			strip_tool="$$( $(WINDOWS_CXX) $(WINDOWS_TARGET_FLAGS) -print-prog-name=strip)"; \
		"$$strip_tool" $(WINDOWS_STRIP_FLAGS) "$(WINDOWS_DISPATCH)"
	@chmod 700 "$(WINDOWS_DISPATCH)"

check-release-macos: check-amalgamated-bytes | prepare-build
	@test "$(HOST_PLATFORM)" = macos || { \
		printf '%s\n' 'error: check-release-macos requires macOS' >&2; exit 2; \
	}
	@build_dir="$${BUILD_DIR:-build}"; \
		BUILD_DIR="$$build_dir" bash scripts/release-macos.sh check 0.0.0 \
		"$$build_dir/release-check/macos"

release-macos: check-amalgamated-bytes | prepare-build
	@test "$(HOST_PLATFORM)" = macos || { \
		printf '%s\n' 'error: release-macos requires macOS' >&2; exit 2; \
	}
	@: "$${RELEASE_VERSION:?set RELEASE_VERSION in the environment}"; \
		build_dir="$${BUILD_DIR:-build}"; \
		release_dir="$${RELEASE_DIR:-$$build_dir/release}"; \
		BUILD_DIR="$$build_dir" bash scripts/release-macos.sh release \
		"$$RELEASE_VERSION" "$$release_dir/macos/$$RELEASE_VERSION"

check-dispatch-header:
	@! grep -nE '^[[:space:]]*#include[[:space:]].*cldmux|cldmux::' apps/dispatch.hpp
	$(CXX) $(CXXFLAGS) -std=$(CXX_STANDARD) -I. \
		-fsyntax-only tests/compile/dispatch.cpp
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-DDISPATCH_NO_MAIN -I. -fsyntax-only apps/dispatch.cpp

check-dispatch: check-dispatch-header dispatch | prepare-build
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -DDISPATCH_TESTING \
		-I. apps/dispatch.cpp tests/dispatch.cpp $(CURL_LIBS) -pthread \
		-o $(DISPATCH_TEST)
	$(DISPATCH_TEST)
	@if test "$(HOST_PLATFORM)" != windows; then sh tests/dispatch.sh $(DISPATCH); fi

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

sanitise: check-amalgamation | prepare-build
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-fsanitize=address,undefined -fno-omit-frame-pointer -I. \
		-isystem "$(TST_DIR)" test.cpp $(CURL_LIBS) -pthread -o $(TEST_SAN)
	$(TEST_SAN)
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) \
		-fsanitize=address,undefined -fno-omit-frame-pointer -I. \
		apps/dispatch.cpp $(CURL_LIBS) -pthread -o $(DISPATCH_SAN)
	sh tests/dispatch.sh $(DISPATCH_SAN)
	$(CXX) $(CXXFLAGS) $(CURL_CXXFLAGS) -std=$(CXX_STANDARD) -DDISPATCH_TESTING \
		-fsanitize=address,undefined -fno-omit-frame-pointer -I. \
		apps/dispatch.cpp tests/dispatch.cpp $(CURL_LIBS) -pthread \
		-o $(DISPATCH_TEST_SAN)
	$(DISPATCH_TEST_SAN)
