CXX      ?= clang++
CXXSTD   := -std=c++20
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
INC      := -Iinclude
RELEASE  := -O3 -DNDEBUG
DEBUG    := -O0 -g
SANITIZE := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all

HEADERS  := $(wildcard include/orderbook/*.hpp)
TEST_SRC := tests/test_main.cpp tests/test_order_book.cpp tests/test_components.cpp tests/test_differential.cpp

.PHONY: all test test-debug sanitize bench replay-demo clean

all: build/tests build/bench build/ob_gen build/ob_replay

build:
	mkdir -p build

build/tests: $(TEST_SRC) $(HEADERS) tests/test_framework.hpp | build
	$(CXX) $(CXXSTD) $(WARN) $(INC) $(RELEASE) $(TEST_SRC) -o $@

build/tests-debug: $(TEST_SRC) $(HEADERS) tests/test_framework.hpp | build
	$(CXX) $(CXXSTD) $(WARN) $(INC) $(DEBUG) $(TEST_SRC) -o $@

build/tests-asan: $(TEST_SRC) $(HEADERS) tests/test_framework.hpp | build
	$(CXX) $(CXXSTD) $(WARN) $(INC) $(SANITIZE) $(TEST_SRC) -o $@

build/bench: bench/bench.cpp $(HEADERS) | build
	$(CXX) $(CXXSTD) $(WARN) $(INC) $(RELEASE) -DOB_BUILD_FLAGS='"$(CXXSTD) $(RELEASE)"' bench/bench.cpp -o $@

build/ob_gen: tools/ob_gen.cpp $(HEADERS) | build
	$(CXX) $(CXXSTD) $(WARN) $(INC) $(RELEASE) tools/ob_gen.cpp -o $@

build/ob_replay: tools/ob_replay.cpp $(HEADERS) | build
	$(CXX) $(CXXSTD) $(WARN) $(INC) $(RELEASE) tools/ob_replay.cpp -o $@

test: build/tests
	./build/tests

test-debug: build/tests-debug
	./build/tests-debug

sanitize: build/tests-asan
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/tests-asan

bench: build/bench
	./build/bench

replay-demo: build/ob_gen build/ob_replay
	./build/ob_gen --ops 1000000 --modify 5 --seed 7 --out build/synthetic_flow.csv
	./build/ob_replay build/synthetic_flow.csv

clean:
	rm -rf build
