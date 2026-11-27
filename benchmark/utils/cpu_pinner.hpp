#ifndef EXSIM_BENCH_CPU_PINNER_HPP
#define EXSIM_BENCH_CPU_PINNER_HPP

#include <string>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace bench {

class CPUPinner {
public:
    // Linux only. macOS has no way to pin a thread to a core: its affinity tag is a scheduler hint.
    static bool pin(int core_id) {
#ifdef __linux__
        if (core_id < 0 || core_id >= CPU_SETSIZE) return false;
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
        (void)core_id;
        return false;
#endif
    }

    static std::string cpu_info() {
#ifdef __APPLE__
        return "Apple Silicon / macOS";
#elif defined(__linux__)
        return "Linux";
#else
        return "Unknown";
#endif
    }
};

}  // namespace bench

#endif
