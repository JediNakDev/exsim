.DEFAULT_GOAL := all
MODE ?= release
CXX ?= c++
BUILD := build/$(MODE)
CPPFLAGS := -I src -I . -MMD -MP
CXXFLAGS := -std=c++23 -Wall -Wextra -Wpedantic -Wshadow $(EXTRA_CXXFLAGS)
# GCC warns that std::hardware_destructive_interference_size depends on -mtune;
# every object in one build shares the same flags, so the value is consistent.
ifneq ($(findstring Free Software Foundation,$(shell $(CXX) --version 2>/dev/null)),)
CXXFLAGS += -Wno-interference-size
# On arm64, GCC 12, 13.1 to 13.3.0 and 14.1 to 14.2.0 can leave ASan fake stack frames marked in use after a return.
# Once a thread has no free frame of one size, each call needing that size scans the full table (about 7 us) and stack
# use after return in those frames goes undetected. Those releases turn the check off. x86-64 builds, as in CI, fixed
# releases and 13.3.1 or 14.2.1 snapshots (which may hold the fix) keep it. The unit test fails on one without the fix.
ifneq ($(filter aarch64%,$(shell $(CXX) -dumpmachine)),)
ifneq ($(filter 12.% 13.1.% 13.2.% 13.3.0 14.1.% 14.2.0,$(shell $(CXX) -dumpfullversion)),)
ASAN_USE_AFTER_RETURN := --param=asan-use-after-return=0
endif
endif
endif
ifeq ($(MODE),debug)
CXXFLAGS += -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined $(ASAN_USE_AFTER_RETURN)
LDFLAGS += -fsanitize=address,undefined
else ifeq ($(MODE),release)
# Release keeps asserts on (no -DNDEBUG) on purpose: a broken invariant aborts. Only perf and bench define NDEBUG.
CXXFLAGS += -O2 -g
else ifeq ($(MODE),fuzz)
# make fuzz: clang's libFuzzer. Every object gets coverage; any sanitizer report stops the run.
CXXFLAGS += -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
LDFLAGS += -fsanitize=fuzzer,address,undefined
else ifneq ($(filter $(MODE),perf bench),)
# bench is the perf configuration under another name, so the benchmarked engine is the perf build.
CXXFLAGS += -O3 -march=native -DNDEBUG -flto -fno-omit-frame-pointer
LDFLAGS += -flto
else
$(error MODE must be release, debug, fuzz, perf or bench)
endif
LDFLAGS += $(EXTRA_LDFLAGS)
LDLIBS := -lpthread
ifeq ($(shell uname -s),Linux)
LDLIBS += -lcrypto
endif
TEST_LIB_SRCS := test/exsim/support/clients/itch_book_builder.cpp test/exsim/support/clients/tcp_client.cpp
LIB_SRCS := $(filter-out src/main.cpp,$(shell find src -name '*.cpp' | sort))
obj = $(patsubst %.cpp,$(BUILD)/obj/%.o,$(1))
LIB_OBJS := $(call obj,$(LIB_SRCS))
TEST_LIB_OBJS := $(call obj,$(TEST_LIB_SRCS))
# A suite is a folder: test/exsim holds unit tests (mirrors src/), test/integration holds sockets and processes.
# Both hold *_tests.cpp only; shared helpers under test/exsim/support are not picked up here.
UNIT_SRCS := $(shell find test/exsim test/framework -name '*_tests.cpp' | sort) test/framework/alloc_counter.cpp
INTEGRATION_SRCS := $(shell find test/integration -name '*_tests.cpp' | sort) test/framework/alloc_counter.cpp
.PHONY: all apps test unit integration clean format format-check
all: apps
apps: $(BUILD)/exsim_exchanged
$(BUILD)/obj/%.o: %.cpp Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD)/exsim_exchanged: $(call obj,src/main.cpp) $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/unit_tests: $(call obj,test/main.cpp $(UNIT_SRCS)) $(LIB_OBJS) $(TEST_LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
unit: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests $(FILTER)
$(BUILD)/integration_tests: $(call obj,test/main.cpp $(INTEGRATION_SRCS)) $(LIB_OBJS) $(TEST_LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
# FILTER goes to both runners (substring for native cases, -k for Python). A runner that matches
# nothing exits 2 (native) or 5 (Python 3.12+); that is not a failure.
integration: apps $(BUILD)/integration_tests $(BUILD)/exsim_wire_bench
	EXSIM_BUILD=$(BUILD) ./$(BUILD)/integration_tests $(FILTER); [ $$? -eq 0 -o $$? -eq 2 ]
	EXSIM_BUILD=$(BUILD) python3 -B -m unittest discover -v -s test/integration -p '*_test.py' $(if $(FILTER),-k '$(FILTER)'); [ $$? -eq 0 -o $$? -eq 5 ]
test: unit integration
clean:
	rm -rf $(BUILD)
# One pinned clang-format for local runs and CI. Generated files are skipped through .clang-format-ignore.
CLANG_FORMAT ?= clang-format-18
FORMAT_SRCS = $(shell git ls-files '*.cpp' '*.hpp')
format:
	$(CLANG_FORMAT) -i $(FORMAT_SRCS)
format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SRCS)
-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null)

# The ITCH replay tool and the benchmarks link only the matching engine and its order book.
ENGINE_OBJS := $(call obj,src/book/order_book.cpp src/engine/matching_engine.cpp)
BENCH_SRCS := $(sort $(wildcard benchmark/*.cpp benchmark/cases/*.cpp benchmark/utils/*.cpp))
BENCH_BUILD ?= build/bench
BENCH_BIN := $(BENCH_BUILD)/exsim_benchmark

ifeq ($(shell uname -s),Darwin)
BENCHMARK_PREFIX := $(shell brew --prefix google-benchmark 2>/dev/null || echo /opt/homebrew/opt/google-benchmark)
BENCH_CPPFLAGS := -I$(BENCHMARK_PREFIX)/include
BENCH_LDFLAGS := -L$(BENCHMARK_PREFIX)/lib
endif

.PHONY: bench bench-build bench-wire itch-replay
$(BUILD)/itch_replay: $(call obj,tools/itch_replay/main.cpp) $(ENGINE_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)
itch-replay:
	$(MAKE) MODE=perf build/perf/itch_replay

$(BUILD)/obj/benchmark/%.o: CPPFLAGS += -I benchmark $(BENCH_CPPFLAGS)
ifeq ($(MODE),bench)
$(BENCH_BIN): $(call obj,$(BENCH_SRCS)) $(ENGINE_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(BENCH_LDFLAGS) -lbenchmark $(LDLIBS)
endif
bench-build:
	$(MAKE) MODE=bench BUILD=$(BENCH_BUILD) $(BENCH_BIN)
bench: bench-build
	$(BENCH_BIN) $(if $(FILTER),--benchmark_filter='$(FILTER)') $(ARGS)

# The wire benchmark is a client of the exsim_exchanged built next to it. It
# needs no Google Benchmark, so any MODE can build it (debug runs it under ASan).
$(BUILD)/exsim_wire_bench: $(call obj,benchmark/wire/main.cpp src/net/socket.cpp src/net/udp.cpp)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)
bench-wire:
	$(MAKE) MODE=bench BUILD=$(BENCH_BUILD) $(BENCH_BUILD)/exsim_exchanged $(BENCH_BUILD)/exsim_wire_bench
	$(BENCH_BUILD)/exsim_wire_bench $(ARGS)

# One libFuzzer harness per parser of untrusted input (fuzz/*.cpp). libFuzzer ships with LLVM clang,
# not Apple clang; clang 19 is the first with std::hardware_destructive_interference_size on libstdc++.
# Each runs FUZZ_SECONDS from its seeds, keeps new inputs in build/fuzz/corpus/<name>, and stops at
# the first crash, leaving the input in build/fuzz/. FILTER picks harnesses by substring. An input runs
# in milliseconds; the 60 s timeout still catches a hang, but not the stalls of up to 37 s seen inside
# ASan's munmap and madvise on a loaded VM.
FUZZ_CXX ?= clang++-19
FUZZ_SECONDS ?= 60
FUZZ_NAMES := $(basename $(notdir $(wildcard fuzz/*.cpp)))
FUZZ_RUN := $(if $(FILTER),$(foreach name,$(FUZZ_NAMES),$(if $(findstring $(FILTER),$(name)),$(name))),$(FUZZ_NAMES))
.PHONY: fuzz fuzz-run
$(FUZZ_NAMES:%=$(BUILD)/fuzz_%): $(BUILD)/fuzz_%: $(BUILD)/obj/fuzz/%.o $(LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
# build/fuzz always, so a BUILD= given here cannot link objects built without coverage or sanitizers.
fuzz:
	$(MAKE) MODE=fuzz CXX=$(FUZZ_CXX) BUILD=build/fuzz fuzz-run
# The config harness starts from the example configs; the others from fuzz/seeds/<name>.
fuzz-run: $(FUZZ_RUN:%=$(BUILD)/fuzz_%)
	for name in $(FUZZ_RUN); do \
		seeds=fuzz/seeds/$$name; [ $$name != config ] || seeds=config; \
		mkdir -p $(BUILD)/corpus/$$name && \
		$(BUILD)/fuzz_$$name -max_total_time=$(FUZZ_SECONDS) -timeout=60 -print_final_stats=1 \
			-artifact_prefix=$(BUILD)/$$name- $(BUILD)/corpus/$$name $$seeds || exit 1; \
	done
