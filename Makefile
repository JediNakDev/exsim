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
endif
ifeq ($(MODE),debug)
CXXFLAGS += -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
else ifeq ($(MODE),release)
# Release keeps asserts on (no -DNDEBUG) on purpose: a broken invariant aborts. Only perf and bench define NDEBUG.
CXXFLAGS += -O2 -g
else ifneq ($(filter $(MODE),perf bench),)
# bench is the perf configuration under another name, so the benchmarked engine is the perf build.
CXXFLAGS += -O3 -march=native -DNDEBUG -flto -fno-omit-frame-pointer
LDFLAGS += -flto
else
$(error MODE must be release, debug, perf or bench)
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
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/unit_tests: $(call obj,test/main.cpp $(UNIT_SRCS)) $(LIB_OBJS) $(TEST_LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
unit: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests $(FILTER)
$(BUILD)/integration_tests: $(call obj,test/main.cpp $(INTEGRATION_SRCS)) $(LIB_OBJS) $(TEST_LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
# FILTER goes to both runners (substring for native cases, -k for Python). A runner that matches
# nothing exits 2 (native) or 5 (Python 3.12+); that is not a failure.
integration: apps $(BUILD)/integration_tests
	EXSIM_BUILD=$(BUILD) ./$(BUILD)/integration_tests $(FILTER); [ $$? -eq 0 -o $$? -eq 2 ]
	EXSIM_BUILD=$(BUILD) python3 -B test/integration/exsim_exchanged_test.py $(if $(FILTER),-k '$(FILTER)'); [ $$? -eq 0 -o $$? -eq 5 ]
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

.PHONY: bench bench-build itch-replay
$(BUILD)/itch_replay: $(call obj,tools/itch_replay/main.cpp) $(ENGINE_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)
itch-replay:
	$(MAKE) MODE=perf build/perf/itch_replay

ifeq ($(MODE),bench)
$(BUILD)/obj/benchmark/%.o: CPPFLAGS += -I benchmark $(BENCH_CPPFLAGS)
$(BENCH_BIN): $(call obj,$(BENCH_SRCS)) $(ENGINE_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(BENCH_LDFLAGS) -lbenchmark $(LDLIBS)
endif
bench-build:
	$(MAKE) MODE=bench BUILD=$(BENCH_BUILD) $(BENCH_BIN)
bench: bench-build
	./$(BENCH_BIN) $(if $(FILTER),--benchmark_filter='$(FILTER)') $(ARGS)
