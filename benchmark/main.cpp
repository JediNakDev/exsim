#include "utils/cpu_pinner.hpp"
#include "utils/csv_writer.hpp"
#include <benchmark/benchmark.h>

#include <iostream>
#include <chrono>
#include <string>

int main(int argc, char** argv) {
    std::cout << "========================================\n";
    std::cout << "Exsim Benchmark Suite\n";
    std::cout << "========================================\n";
    std::cout << "CPU: " << bench::CPUPinner::cpu_info() << "\n";
    std::cout << "Compiler: " << __VERSION__ << "\n";
#ifdef NDEBUG
    std::cout << "Asserts: off\n";
#else
    std::cout << "Asserts: on\n";
#endif

    auto resolution = std::chrono::nanoseconds::max();
    for (int i = 0; i < 10000; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
        if (elapsed.count() > 0 && elapsed < resolution) resolution = elapsed;
    }
    std::cout << "Timer: steady_clock, minimum observed positive delta " << resolution.count()
              << " ns; batches of 32 commands\n";

    int target_core = 0;
    std::string csv_path = "build/bench/exsim_results.csv";

    int benchmark_argc = 1;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("--core=") == 0) {
            target_core = std::stoi(arg.substr(7));
        } else if (arg.find("--csv=") == 0) {
            csv_path = arg.substr(6);
        } else {
            argv[benchmark_argc++] = argv[i];
        }
    }

    argc = benchmark_argc;
    argv[argc] = nullptr;

    if (bench::CPUPinner::pin(target_core)) {
        std::cout << "CPU Pinned: Core " << target_core << "\n";
    } else {
        std::cout << "CPU Pinning: Not available\n";
    }

    bench::csv() = std::make_unique<bench::CSVWriter>(csv_path);
    std::cout << "CSV Output: " << csv_path << "\n";
    std::cout << "========================================\n\n";

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    if (benchmark::RunSpecifiedBenchmarks() == 0) return 1;
    benchmark::Shutdown();

    bench::csv().reset();
    std::cout << "\nResults: " << csv_path << "\n";

    return 0;
}
