// On arm64, GCC 12, 13.1 to 13.3.0 and 14.1 to 14.2.0 can leave ASan fake stack frames marked in use after a return.
// Once a thread has no free frame of one size, each call needing that size scans the full table (about 7 us) and stack
// use after return in those frames goes undetected. The Makefile turns the check off there. This case fails if a build
// runs it on a fake stack that fills up.
#include "test.hpp"

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>

#include <thread>

namespace {
// Always false on macOS: its runtime reports no fake stack unless detect_stack_use_after_return=1 is set.
[[gnu::noinline]] bool local_is_on_fake_stack() {
    volatile int local = 0;
    void* const fake_stack = __asan_get_current_fake_stack();
    return fake_stack != nullptr &&
           __asan_addr_is_in_fake_stack(fake_stack, const_cast<int*>(&local), nullptr, nullptr) != nullptr;
}
}  // namespace

TEST_CASE(test_framework, asan_fake_stack_does_not_fill_up) {
    bool first = false;
    bool last = false;
    // A new thread starts with an empty fake stack, whatever earlier cases left on this one.
    std::thread([&] {
        first = local_is_on_fake_stack();
        for (int i = 0; i < 100000; ++i) (void)local_is_on_fake_stack();
        last = local_is_on_fake_stack();
    }).join();
    CHECK_EQ(last, first);
}
#endif
