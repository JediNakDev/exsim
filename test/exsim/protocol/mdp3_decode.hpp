#ifndef TEST_EXSIM_PROTOCOL_MDP3_DECODE_HPP
#define TEST_EXSIM_PROTOCOL_MDP3_DECODE_HPP
// Typed MDP 3.0 decoders. Production code never decodes MDP messages; only tests do.
#include <protocol/mdp3.hpp>

namespace exsim::protocol::mdp3 {

struct IncrementalView {
    std::uint64_t transact_time = 0;
    std::uint8_t event_indicator = 0;
    cme::GroupView entries;
    cme::GroupView orders;
};

// Applies only to schema-13 MBP book (46) and trade summary (48).
[[nodiscard]] inline Status decode_incremental(const cme::MessageView& message, IncrementalView& out) noexcept {
    const auto& header = message.header;
    if (header.schema_id != schema::id || header.version != schema::version ||
        (header.template_id != 46 && header.template_id != 48))
        return Status::invalid_type;
    if (header.block_length < 11 || message.body.size() < header.block_length) return Status::invalid_length;
    IncrementalView view;
    view.transact_time = cme::load_le<std::uint64_t>(message.body.data());
    view.event_indicator = static_cast<std::uint8_t>(message.body[8]);
    if (cme::decode_group(message.trailing(), 32, view.entries) != Status::ok) return Status::invalid_length;
    auto bytes = message.trailing().subspan(view.entries.encoded_size);
    if (bytes.size() < 8) return Status::invalid_length;
    const auto block = cme::load_le<std::uint16_t>(bytes.data());
    const auto count = static_cast<std::uint8_t>(bytes[7]);
    if (block < (header.template_id == 46 ? 24 : 16)) return Status::invalid_length;
    const std::size_t size = std::size_t{block} * count;
    if (size != bytes.size() - 8) return Status::invalid_length;
    view.orders = {block, count, bytes.subspan(8), size + 8};
    out = view;
    return Status::ok;
}

[[nodiscard]] inline Status decode_snapshot(const cme::MessageView& message, Snapshot& out,
                                            cme::GroupView& entries) noexcept {
    const auto& header = message.header;
    if (header.schema_id != schema::id || header.version != schema::version || header.template_id != 52)
        return Status::invalid_type;
    if (header.block_length < 59 || message.body.size() < header.block_length) return Status::invalid_length;
    cme::GroupView group;
    if (cme::decode_group(message.trailing(), 22, group) != Status::ok ||
        group.encoded_size != message.trailing().size())
        return Status::invalid_length;
    const char* root = message.body.data();
    out = {cme::load_le<std::uint32_t>(root),      cme::load_le<std::uint32_t>(root + 4),
           cme::load_le<std::int32_t>(root + 8),   cme::load_le<std::uint32_t>(root + 12),
           cme::load_le<std::uint64_t>(root + 16), cme::load_le<std::uint64_t>(root + 24),
           cme::load_le<std::uint16_t>(root + 32), static_cast<std::uint8_t>(root[34]),
           cme::load_le<std::int64_t>(root + 35),  cme::load_le<std::int64_t>(root + 43),
           cme::load_le<std::int64_t>(root + 51)};
    entries = group;
    return Status::ok;
}

}  // namespace exsim::protocol::mdp3

#endif
