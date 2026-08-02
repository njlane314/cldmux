CXX ?= c++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror
CURL_FLAGS := $(shell curl-config --cflags --libs)
TEST := /tmp/cloud-hpp-test

.PHONY: check sanitize
check:
	awk '/^```cpp$$/ {code=1; next} code && /^```/ {exit} code {print}' README.md | \
		$(CXX) $(CXXFLAGS) -I. -x c++ -fsyntax-only -
	$(CXX) $(CXXFLAGS) -I. test.cpp $(CURL_FLAGS) -pthread -o $(TEST)
	$(TEST)

sanitize:
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I. test.cpp $(CURL_FLAGS) -pthread -o $(TEST)-san
	$(TEST)-san
