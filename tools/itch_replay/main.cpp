// itch_replay: replay one stock of a NASDAQ ITCH 5.0 file through the
// matching engine and report throughput + final book state.
//
// Usage:
//   itch_replay <file.itch> --stock SYMBOL   # replay SYMBOL from a NASDAQ file
//   itch_replay --synth N                    # replay N synthetic messages
//   add --validate to diff the book against a reference book after every message
//
// The file is NASDAQ's format: each ITCH 5.0 message follows its 2-byte
// big-endian length. Book messages (A, F, E, C, X, D, U) for SYMBOL's stock
// locate, named by its first R message, change the book; every other message
// is skipped by its length. An empty message, a file that ends inside a
// message, or no R message for SYMBOL is an error. The whole file is read
// into memory. The replay book covers $0.0001 to $200; adds outside it are
// counted as rejected, and a replace to a price outside it retires the order.

#include "diff.hpp"
#include "itch_replayer.hpp"
#include "reference_book.hpp"
#include "synthetic.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

std::vector<char> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        std::exit(1);
    }
    auto size = in.tellg();
    in.seekg(0);
    std::vector<char> buf(static_cast<std::size_t>(size));
    if (!in.read(buf.data(), size)) {
        std::cerr << "cannot read " << path << "\n";
        std::exit(1);
    }
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    bool synth = false;
    bool validate = false;
    std::size_t synth_n = 0;
    std::string path;
    std::string stock;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--synth" && i + 1 < argc) {
            synth = true;
            synth_n = std::stoull(argv[++i]);
        } else if (a == "--stock" && i + 1 < argc) {
            stock = argv[++i];
        } else if (a == "--validate") {
            validate = true;
        } else {
            path = a;
        }
    }
    if (!synth && (path.empty() || stock.empty() || stock.size() > 8)) {
        std::cerr << "usage: itch_replay <file.itch> --stock SYMBOL [--validate] | --synth N [--validate]\n";
        return 1;
    }

    std::vector<char> wire;
    if (synth) {
        exsim::itch_replay::SyntheticItchStream gen(0xC0FFEE, synth_n, /*mid=*/10000);
        wire = gen.build();
    } else {
        wire = read_file(path);
    }

    exsim::itch_replay::ReferenceBook ref;
    exsim::itch_replay::ItchReplayer replayer(exsim::itch_replay::max_live_orders_for(wire.size()),
                                              validate ? &ref : nullptr,
                                              synth ? std::nullopt : std::optional{exsim::Symbol(stock)});
    const exsim::MatchingEngine& engine = replayer.engine();

    std::size_t parsed = 0;
    std::size_t skipped = 0;
    std::size_t mismatches = 0;

    auto t0 = std::chrono::steady_clock::now();
    const bool complete = exsim::itch_replay::for_each_message(wire, [&](std::span<const char> message) {
        if (!replayer.apply(message)) {
            ++skipped;
            return;
        }
        ++parsed;
        if (validate) {
            auto d = exsim::itch_replay::diff_books(engine, replayer.instrument(), ref, 10);
            if (!d.ok && ++mismatches <= 3) {
                std::cerr << "DIVERGENCE @msg " << parsed << ": " << d.detail << "\n";
            }
        }
    });
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    auto const& s = replayer.stats();
    std::cout << "file_bytes=" << wire.size() << " parsed=" << parsed << " skipped=" << skipped << " elapsed_s=" << secs
              << " msg_per_sec=" << (parsed / secs) << "\n"
              << "adds=" << s.adds << " executes=" << s.executes << " cancels=" << s.cancels << " deletes=" << s.deletes
              << " replaces=" << s.replaces << " unknown_ref=" << s.unknown_ref << " dropped_cross=" << s.dropped_cross
              << " rejected=" << s.rejected << "\n";
    if (validate) std::cout << "validation_mismatches=" << mismatches << "\n";

    auto bid = engine.best_bid(replayer.instrument());
    auto ask = engine.best_ask(replayer.instrument());
    std::cout << "best_bid=" << (bid ? std::to_string(*bid) : "-") << " best_ask=" << (ask ? std::to_string(*ask) : "-")
              << " live_orders=" << engine.live_order_count() << "\n";
    if (!complete) {
        std::cerr << "the file has an empty message or ends inside one\n";
        return 1;
    }
    if (!synth && !replayer.stock_found()) {
        std::cerr << "no R message names " << stock << "\n";
        return 1;
    }
    return (validate && mismatches > 0) ? 2 : 0;
}
