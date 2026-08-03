CXX ?= c++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror
CURL_FLAGS := $(shell curl-config --cflags --libs)
TST_DIR ?= ../tst
TEST := /tmp/cloud-h-test
EXAMPLE := /tmp/cloud-h-example

.PHONY: check example sanitize
check: example $(TST_DIR)/tst.hpp
	awk '/^```cpp$$/ {code=1; next} code && /^```/ {exit} code {print}' README.md | \
		$(CXX) $(CXXFLAGS) -I. -x c++ -fsyntax-only -
	$(CXX) $(CXXFLAGS) -I. -I"$(TST_DIR)" test.cpp $(CURL_FLAGS) -pthread -o $(TEST)
	$(TEST)

example:
	$(CXX) $(CXXFLAGS) -I. example.cpp $(CURL_FLAGS) -pthread -o $(EXAMPLE)

sanitize: $(TST_DIR)/tst.hpp
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I. -I"$(TST_DIR)" test.cpp $(CURL_FLAGS) -pthread -o $(TEST)-san
	$(TEST)-san
