#ifndef EXSIM_BENCH_CSV_WRITER_HPP
#define EXSIM_BENCH_CSV_WRITER_HPP

#include "utils/stats.hpp"

#include <fstream>
#include <iomanip>
#include <memory>
#include <string>

namespace bench {

class CSVWriter {
public:
    explicit CSVWriter(const std::string& filename) : file_(filename) {
        if (file_.is_open()) {
            file_ << "Benchmark,Operations,Mean_ns,P50_ns,P95_ns,P99_ns,P99.9_ns,P99.99_ns,"
                     "Min_ns,Max_ns,Throughput_ops_per_sec\n";
        }
    }

    void write(const std::string& name, const Stats& s) {
        if (!file_.is_open()) return;
        file_ << name << "," << s.operations << "," << std::fixed << std::setprecision(2)
              << s.mean_ns << "," << s.p50_ns << "," << s.p95_ns << "," << s.p99_ns << ","
              << s.p999_ns << "," << s.p9999_ns << "," << s.min_ns << "," << s.max_ns << ","
              << std::setprecision(0) << s.throughput_per_second << "\n";
    }

private:
    std::ofstream file_;
};

inline std::unique_ptr<CSVWriter>& csv() {
    static std::unique_ptr<CSVWriter> instance;
    return instance;
}

}  // namespace bench

#endif
