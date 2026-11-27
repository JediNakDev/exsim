// Replays a journal into a fresh matching engine and checks that every command
// produces the events the live exchange produced, by the output digest each
// record carries. Stops at the first command that differs and prints it.
//
//   journal_replay PATH
//
// Exit status: 0 every output matches, 1 a divergence or an unreadable journal,
// 2 bad arguments.

#include <server/journal/format.hpp>
#include <server/journal/journal.hpp>

#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>

using namespace exsim;
using namespace exsim::server::journal;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: journal_replay PATH\n";
        return 2;
    }
    try {
        JournalReader journal(argv[1]);
        MatchingEngine engine(journal.setup().engine);
        for (const InstrumentConfig& instrument : journal.setup().instruments) {
            if (engine.add_instrument(instrument) == 0)
                throw std::runtime_error("instrument " + std::string(instrument.symbol.view()) + " not accepted");
        }
        EventBuffer events(required_event_capacity(journal.setup().engine));
        // Once the header and footer check out, each record is replayed as it is read, so the
        // first problem among the records is reported in sequence order and memory does not
        // grow with the journal.
        std::uint64_t sequence = 0, event_count = 0;
        while (const std::optional<JournalRecord> record = journal.next()) {
            events.clear();
            const bool applied = engine.handle(record->command, events) == HandleResult::Applied;
            const std::span<const Event> replayed(events.begin(), events.size());
            if (!applied || output_digest(replayed) != record->output) {
                std::cout << "divergence at sequence " << sequence << ": " << format_command(record->command) << '\n';
                std::cout << "journal output digest " << std::hex << record->output << ", replay "
                          << output_digest(replayed) << std::dec << " from " << replayed.size() << " events:\n";
                for (const Event& event : replayed) std::cout << "  " << format_event(event) << '\n';
                return 1;
            }
            event_count += replayed.size();
            ++sequence;
        }
        std::cout << "replayed " << sequence << " commands and " << event_count
                  << " events: every output matches the journal\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "journal_replay: " << argv[1] << ": " << error.what() << '\n';
        return 1;
    }
}
