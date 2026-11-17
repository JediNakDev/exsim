# TODO

Open findings from the code review of `e5045790`, checked by a second reviewer. Numbers are stable so issues and commits can refer to them; 16, 63 and 73 were withdrawn.
Memory figures are computed from the code, not measured.

## Fix these first

- [x] **1.** **Phase and fault control is unreachable in the shipped product.** `CoreAdmin` and [admin_command.cpp](./lib/src/server/admin/admin_command.cpp) link only into test binaries (`TEST_LIB_SRCS` in the Makefile). The control socket in [src/exchange/main.cpp](./src/exchange/main.cpp) accepts only `status` and `stop`, and the HTTP API has only lifecycle endpoints. No operator can send `SetPhase` or set a user or packet fault at runtime (the only production `SetPhase` is the close-all at shutdown, `exchange_core.hpp:614`), so auctions, uncross, halts and fault injection are production code that only tests can trigger. An instrument with `initial_phase = pre_open` stays there until end-of-day close. Fixed in #8: `POST /v1/exchanges/{id}/admin`.
- [ ] **2.** **`OrderBook` is two classes in one.** It is a standalone matching book (`add_order`, `cancel_order`, `modify_order`, `match_order_impl`, `get_snapshot`) and also passive storage for the engine (`rest_order`, `execute_order`, `amend_order`, `remove_order`). The engine sets `max_fills_per_add_order = 1; // the engine never calls add_order`. The `std::shared_ptr<OrderBookStorage>` exists only to serve both roles: the standalone book owns its storage, the engine shares one across its books.
- [ ] **3.** **Benchmarks measure the path production does not use.** Every case drives `OrderBook::add_order` and friends. Nothing measures `MatchingEngine`, a gateway, or the reactor. `benchmark/utils/sharded_engine` is a multi-threaded engine that exists only in the benchmark, while production is single-threaded.
- [ ] **4.** **Per-operation percentiles are at clock resolution.** `BenchmarkRunner::run` wraps each operation in two `high_resolution_clock::now()` calls ([runner.hpp:29](./benchmark/utils/runner.hpp#L29)). The last committed CSV (`results/benchmark_result_0.1.3.csv`) shows AddOrder P50 41, P95 42, P99 42 and CancelOrder P50 0.00, which is the roughly 42 ns timer tick. Only `sharded_throughput.cpp` times a batch. `benchmark/utils/cycle_timer.hpp` (rdtsc) exists and is unused. Needs a fresh run to confirm on current code.
- [ ] **5.** **Two networking stacks.** [http_server.cpp](./lib/src/api/http_server.cpp) uses raw `int` descriptors, manual cleanup and a thread per connection. It bypasses the project's own RAII `net::Socket` and `Poller` and duplicates socket options (`TCP_NODELAY` at [http_server.cpp:38](./lib/src/api/http_server.cpp#L38) and [socket.cpp:92](./lib/src/net/socket.cpp#L92)). This is defensible for a management plane, but the trade-off is not documented.
- [x] **6.** **README links were broken.** Fixed: the TEST and BENCHMARK links now point to `./test/README.md` and `./benchmark/README.md`. The typo "protocal" on line 3 is left as is.

## Architecture

- [x] **7.** **Journal is half-integrated.** Managed exchanges reject it ([exchange_service.cpp:36](./lib/src/api/exchange_service.cpp#L36)), so it works only with standalone `exsim_exchanged --config`. There is no `fsync`, so "flush" reaches the page cache only. `decode_command` has only test callers, so the product cannot replay its own journal. Fixed in #13: managed exchanges accept `[journal]` with a file name in the data directory; `flush()` is documented as page cache only.
- [x] **8.** **Journal record format is wasteful.** Every command is a fixed 280-byte record, so a 3-field `SetPhase` costs the same as a 12-field `NewOrder`. `flush()` writes a 24-byte footer that the next `append` seeks back over. Fixed in #13: schema 2 records are 56 to 128 bytes. The 24-byte flush footer stays.
- [ ] **9.** **Core depends on the market data layer.** `server/core/policies.hpp` includes `md/publisher.hpp` and `md/packet_faults.hpp` for a few types.
- [ ] **10.** **Codec layer depends on the engine.** `protocol/wire.hpp` includes `engine/instrument.hpp`.
- [ ] **11.** **`InstrumentConfig` carries CME identity fields** (`cme_security_id`, `cme_symbol[20]`, maturity, contract size) inside the core config. They belong in a per-protocol mapping.
- [ ] **12.** **`api/` mixes two things.** It holds the management service and also `api_json` and `runtime_snapshot`, which the exchange daemon includes.
- [ ] **13.** **Defaults reserve for the worst case.** Computed at default config:
    - Event buffer: 2,097,284 events at 104 B, 208 MiB (reserved, untouched until large batches).
    - Session dispatch positions: 16 MiB.
    - Each OUCH user: 140 MiB, zero-initialised at startup.
    - Retransmission ring: 64 MiB.
    - TCP buffers: 4 MiB send plus 64 KiB receive per slot, times 64 slots, 260 MiB per enabled gateway.
- [ ] **14.** **`MatchingEngine(const EngineConfig& = {})`** silently allocates for 1M orders through a default argument.
- [ ] **15.** **Two quantity types.** `book::Quantity` is `uint64_t` and `exsim::Quantity` is `uint32_t`, with casts throughout the engine.
- [ ] **17.** **Same policy name, different meaning.** `MdpRuntime::flush()` flushes only the TCP replay server; `ItchRuntime::flush()` flushes the publisher.

## Folder and file placement

- [ ] **18.** `engine/types.hpp` calls itself the core vocabulary for every subsystem. Move it to a `common/` or `core/` folder.
- [ ] **19.** `engine/price_ladder.hpp` now defines only `InstrumentBook`. Rename it.
- [ ] **20.** `net/clocks.hpp` holds time utilities, not networking. Same for `engine/price_text.hpp` (formatting).
- [ ] **21.** `md/random.hpp` and `md/packet_faults.hpp` are generic simulation utilities living in `md/`.
- [ ] **22.** `md/publisher.hpp` is ITCH-specific; its sibling is `mdp_publisher.hpp`. Name it `itch_publisher.hpp`.
- [ ] **23.** `server/exchange_core.hpp` and `server/server_runtime.hpp` sit at the `server/` root while their parts live in `server/core/` and `server/runtime/`.
- [x] **24.** `server/admin/*` lives in `lib/` but is test-only. Ship it (see 1) or move it under `test/`. Shipped in #8.
- [ ] **25.** `src/utils/itch_replay` and `src/utils/cme_codegen` are tools sitting beside the two real binaries. A `tools/` folder reads better.
- [ ] **26.** Gateway file granularity is inconsistent: OUCH is one 1036-line `ouch_server.hpp`, FIX is five files plus a 15-line umbrella `fix_gateway.hpp`, iLink is two files.
- [ ] **27.** The unit versus integration split exists only as path lists in the Makefile. `test/integration/` holds just the Python API test, while C++ integration tests sit in `test/exsim/net/` and a few listed files.
- [ ] **28.** Test file naming has four conventions: `*_tests.cpp`, `*_test.cpp` (all OUCH), no suffix (`test/exsim/protocol/`), and process names (`fix_review_tests.cpp`, `core_review_tests.cpp`).
- [ ] **29.** `results/` and `benchmark/profiling/macos_capture/` hold stale data that their own READMEs disclaim.
- [ ] **30.** `test/README.md` still refers to an ignored `legacy/` folder.
- [ ] **31.** Include guard style is mixed: seven `benchmark/utils` headers, `ilink_test_support.hpp` and the generated schema use `#pragma once`; everything else uses include guards.

## Unused, test-only, single-use, duplicated

- [ ] **32.** **No users at all:** ~~`kAllEventTypes`~~ (removed in #13), `book::Timestamp`, `cycle_timer.hpp`, and `BenchmarkRunner::set_samples`, `add_counter`, `latencies`, `state`.
- [ ] **33.** **Test-only code in `lib/`:**
    - ~~`fix44::FrameReader` and the typed `fix44::decode` functions~~ (moved to `test/` in #14)
    - ~~`moldudp64::PacketReader`~~ (moved to `test/` in #14)
    - ~~journal `decode_command` and `hash_event`~~ (moved to `test/` in #13)
    - `ServerConfig::find_user`, `utc_midnight_ns`
    - `FlatHashMap::for_each` and `max_entries`
    - socket helpers `start_connect`, `connect_result`, `peer_endpoint`, `get_send_buffer_size`, `get_receive_buffer_size`
- [ ] **34.** **Engine queries with no production caller:** `level_orders`, `for_each_order`, `for_each_level_order`, `best_bid`, `best_ask`. (`depth` is used by the MDP publisher, `mdp_publisher.hpp:135`.)
- [ ] **35.** **A global order list is maintained for nobody.** `all_orders` is relinked on every rest and retire ([matching_engine.cpp:320](./lib/src/engine/matching_engine.cpp#L320)) and read only by `for_each_order` (line 1300), which has no production caller.
- [ ] **36.** **`find_order(OrderId)` has one production caller**, [ilink_user.hpp:439](./lib/include/gateway/ilink/ilink_user.hpp#L439). OUCH and FIX flows still pay an OrderId hash insert and erase per order.
- [x] **37.** **Two FIX inbound decoders.** Production uses `Parser` plus `FixFieldReader`; `fix44::decode` is exercised only by tests. Fixed in #14: typed decoders moved to `test/exsim/protocol/fix44_decode.hpp`.
- [ ] **38.** **Duplicates:**
    - Nanoseconds-per-day is defined three times (`server/config.hpp:82`, `net/clocks.hpp`, `md/feed_common.hpp`).
    - ~~`fix44::parse_price` and `server::parse_price`.~~ (both call `parse_decimal_price` since #14)
    - `IncomingOrder` is filled field by field in both `on_new_order` and `on_replace`.
    - `__builtin_ctzll` (matching_engine.cpp:916, 933) alongside `std::countr_zero` in `ActiveBitmap`.
- [ ] **39.** **Single-use one-line wrappers:** `listen_for_connections` and `accept_connection` each have one production caller.
- [ ] **40.** **Dead check:** `valid_instrument_config` ends with `levels <= numeric_limits<size_t>::max()` ([instrument.hpp:63](./lib/include/engine/instrument.hpp#L63)), which is always true on the supported 64-bit targets.
- [ ] **41.** `PriceLevel::pop_front` and `get_snapshot` exist only for the standalone path (see 2).

## Cache friendliness

- [ ] **42.** Per-order state is spread over `Order` (64 B), `OrderHot` (32 B), `OrderCold` (24 B), `InstrumentListLinks` (8 B) and two hash slots. Three doubly linked lists are updated on every rest and retire.
- [ ] **43.** Reaching the best order takes two dependent loads: ladder slot, then pooled `PriceLevel`, then `Order`. Storing levels inline in the ladder removes one.
- [ ] **44.** `ActiveBitmap` uses `vector<vector<uint64_t>>` for at most four layers. A flat array with layer offsets avoids the extra indirection.
- [ ] **45.** `Event` is 104 B because of `FillEvent` (88 B) and `ReplacedEvent` (80 B). A `Rested` or `PhaseChanged` event pays the same.
- [ ] **46.** `rest_order` probes the hash twice: `contains` at [order_book.cpp:309](./lib/src/book/order_book.cpp#L309), then `insert` at 319. `insert` already reports duplicates.
- [ ] **47.** Best prices are cached twice, in the engine (`best_bid_tick`, `best_ask_tick`) and in the book.
- [ ] **48.** `InstrumentBook` holds the cold `InstrumentConfig` (including `cme_symbol[20]`) ahead of its hot fields.
- [ ] **49.** TCP buffers are `new char[capacity]` (`stream_buffers.hpp:42, 109`), so they are first touched on the hot path. The engine structures prefault; these do not.
- [ ] **50.** The ITCH publisher encodes to a stack buffer, copies into the packet, then copies into the ring. It also walks each batch twice (`validate_batch`, then `publish_event`).
- [ ] **51.** `SPSCQueue` hardcodes `alignas(128)` instead of `std::hardware_destructive_interference_size`, keeps its buffer inside the object, and is copy-only.
- [x] **52.** `moldudp64::PacketBuilder::append` and the OUCH field copies go byte by byte. `wire::load/store` rely on the optimiser to form a bswap, which is worth confirming in a profile. Bulk copies done in #14; the bswap check stays with the profiling work in #9.

## Data structures and complexity

- [ ] **53.** `std::unordered_map` is used for symbol lookup ([matching_engine.cpp:143](./lib/src/engine/matching_engine.cpp#L143)) and for iLink security ids ([ilink_user.hpp:39](./lib/include/gateway/ilink/ilink_user.hpp#L39), looked up per order at 362). `FlatHashMap` already exists.
- [ ] **54.** `plan_uncross` scans occupancy words linearly across the crossed range instead of using the bitmap hierarchy.
- [ ] **55.** `run_timers` walks every open connection on every loop iteration, and with `busy_poll` that is every spin. A next-deadline value or timer wheel makes it O(1) when nothing is due.
- [ ] **56.** `poll_once` runs every endpoint's timers and flush each iteration, whether or not anything changed.
- [ ] **57.** The FIX `Parser` detects duplicates with a linear scan per field (O(n²), n at most 64) and `get(tag)` scans per lookup. `find_duplicate_tag` scans a 256-entry array.
- [ ] **58.** `fix44::Builder::finish` memmoves the body because 14 header bytes are reserved but the BodyLength digit count varies ([fix44.hpp:263](./lib/include/protocol/fix44.hpp#L263)). Measure before changing.
- [ ] **59.** `RetransmissionRing` and `DeferredCommandQueue` index with `% capacity_`. A power-of-two capacity with a mask avoids the division, as `SPSCQueue` already does.
- [x] **60.** `JournalWorker` polls and sleeps 1 ms when empty, so there is up to 1 ms of added latency and no wake mechanism (eventfd, futex or condition variable). Its destructor waits 100 ms and then detaches a stuck writer. Fixed in #13: atomic wait and notify. The 100 ms bounded shutdown stays.
- [ ] **61.** `publish_imbalances` loops all `max_instruments` once per interval, and a second time when any instrument is in a call phase. `rested_replacement` forward-scans per priority-losing replace.
- [ ] **62.** `FlatHashMap::clear` and `TokenTable::clear` cost O(capacity), not O(size).
- [ ] **64.** Latency stats keep a `vector<double>` and sort it. A histogram gives fixed memory and O(1) recording.

## Modern C++

- [ ] **65.** `std::byte` has zero uses. All wire buffers are `char` or `unsigned char`.
- [ ] **66.** `Command` and `Event` are hand-rolled tagged unions, although `std::variant` is used well in `gateway_sessions.hpp` and `market_data_runtime.hpp`. If hand-rolling is deliberate for layout, say so in a comment.
- [ ] **67.** `ts` lives in each `Command` payload, so `Command::ts()` and `ExchangeCore::set_command_timestamp` each need a five-way switch. One header field removes both.
- [ ] **68.** `EXSIM_LIKELY`/`EXSIM_UNLIKELY` macros have 59 uses; `[[likely]]`/`[[unlikely]]` have none.
- [ ] **69.** `std::thread` is used everywhere. `std::jthread` fits the threads that are always joined (HTTP server, process watcher). The journal worker should stay as is: `jthread` cannot interrupt blocked file I/O and its joining destructor would lose the bounded shutdown.
- [ ] **70.** `const_cast` sites: [flat_hash_map.hpp:82](./lib/include/util/flat_hash_map.hpp#L82), `tcp_server.hpp:234`, `exchange_control.cpp:31`, `exchange_process.cpp:42`.
- [ ] **71.** `ObjectPool::destroy` uses `reinterpret_cast<Node*>` without `std::launder` ([object_pool.hpp:77](./lib/include/util/object_pool.hpp#L77)). Growth defaults to on in the pool and off in `FlatHashMap`.
- [ ] **72.** `for_each_order` uses a function pointer plus `void*` trampoline. `order_info` uses `memset`. The engine constructor uses a comma-expression trick to validate config.
- [ ] **74.** `std::expected` is unused. `protocol::Status` ([wire.hpp:11](./lib/include/protocol/wire.hpp#L11)) has no fixed underlying type. `Status` is `std::uint8_t` since #14; `std::expected` is still open.
- [ ] **75.** `book/types.hpp` uses unqualified `uint8_t`. `mdp_runtime.hpp` and `journal.hpp` rely on transitive includes.
- [ ] **76.** `.clang-format` says `Standard: c++20` for a C++23 project, and several files do not follow it (`book/*`, `server/journal/*`, `mdp_runtime.hpp`, `gateway/ilink/*`, the iLink block in `order_entry_gateways.hpp`, `src/api/main.cpp`).
- [ ] **77.** Naming is mixed: `get_best_bid` in the book, `best_bid` in the engine.
- [ ] **78.** `std::hive` is C++26, so it is not available here. `ObjectPool` plus intrusive lists is the equivalent choice; one sentence in `docs/dsa.md` would answer the "why not hive" question.
- [ ] **79.** The hand-written JSON, HTTP parser, INI parser and test framework each need a short "why not a library" note.

## Linux and networking

- [ ] **80.** The poller is level-triggered with one `recv` per readiness event. That is a fine choice, but the reason over edge-triggered is not written down.
- [ ] **81.** `socket()` and `accept()` are followed by `fcntl`. On Linux, `SOCK_NONBLOCK | SOCK_CLOEXEC` and `accept4` save the extra syscalls.
- [ ] **82.** kqueue `change()` issues two `kevent` calls where one changelist would do.
- [ ] **83.** There is no scatter I/O (`writev`, `sendmsg`) and no UDP batching (`sendmmsg`, `recvmmsg`).
- [ ] **84.** UDP send results are discarded in `itch_runtime.hpp`.
- [ ] **85.** The stack is IPv4 only, with no `SO_REUSEPORT`.
- [x] **86.** Signal handling differs between the two binaries: `std::signal` with `volatile sig_atomic_t` in the exchange daemon, `sigwait` in the API. The daemon also closes its raw control fd by hand. Fixed in #8.

## Benchmarking and profiling

- [ ] **87.** `Stats::throughput = 1e9 / mean` includes the clock overhead.
- [ ] **88.** Bench mode adds `-DEXSIM_BOOK_DETERMINISTIC_POOL`, so the benchmarked book differs from the release build.
- [ ] **89.** `mixed_workload.cpp` calls `stats.report(state)` inside the iteration loop.
- [ ] **90.** No current numbers and no flame graph are committed. `run_perf.sh` exists, but the only captures are from 0.1.4 and are marked as not current.

## Testing, CI, build

- [ ] **91.** The in-house test framework has no fixtures, parametrised cases or per-test isolation.
- [ ] **92.** `test/fixtures/exchange.conf` uses small limits (`max_orders = 4096`), so no test exercises default-sized allocation (see 13).
- [ ] **93.** `make integration FILTER=...` is not forwarded to the Python API test ([Makefile:54](./Makefile#L54)); the test itself supports `-k`.
- [ ] **94.** CI has no ThreadSanitizer job despite three threaded components (journal worker, HTTP API, process watcher). It also has no format check and does not build the benchmarks.
- [ ] **95.** The release build is `-O2 -g` without `-DNDEBUG`, so asserts stay on. If that is intentional, say so in the Makefile.

## Added after verification

- [x] **97.** The journal header omits CME identity. [journal.cpp:28](./lib/src/server/journal/journal.cpp#L28) writes each instrument only up to `initial_phase`, so `cme_security_id`, `cme_symbol`, maturity and contract size cannot be reconstructed from a journal. Fixed in #13.

## Second pass: files the first review skipped

From the second reviewer's read of the gateway, market data, protocol, config and API files. 98, 99, 101, 103, 105, 106 and 109 were rechecked against the code; the rest are that reviewer's alone. No builds or tests were run for these.

- [ ] **98.** **Accepted configuration can expose credentials in API responses.** `lib/src/api/exchange_service.cpp:51` trims only spaces and tabs before redacting keys, whereas `lib/src/server/config.cpp:20` also trims carriage returns: `password\r=demo` is accepted as a password assignment but returned unchanged by `redacted()`, including through list and create responses.
- [x] **99.** **FIX cancels and replaces ignore the supplied instrument and side.** `lib/include/gateway/fix/fix_connection.hpp:290` and `:304` validate tags 55 and 54 then discard their values; the requests passed to `FixUserState` contain neither, so a request naming MSFT and Sell can cancel or amend an AAPL Buy identified by OrigClOrdID. Fixed in #21: a mismatch gets OrderCancelReject, unknown order.
- [x] **100.** **OUCH shutdown can terminate a replay before its remaining reports.** `lib/include/gateway/ouch/ouch_server.hpp:828` calls `deliver_stored_messages()` once, then writes End of Session and closes even if replay paused for space; when three bytes remain but the next stored frame does not fit, the client receives `Z` after an incomplete history. Fixed in #21: no End of Session while replay is incomplete.
- [x] **101.** **An iLink transport overflow permanently blocks the user from reconnecting.** `lib/include/gateway/ilink/ilink_user.hpp:269` sets the day-lifetime `output_failed` flag when the connection sink is full, even after successfully storing the report; authentication checks `!candidate->output_failed` at `lib/include/gateway/ilink/ilink_connection.hpp:102`, and neither close nor negotiation clears it, so reconnect-and-replay cannot recover from this transient overload. Fixed in #21: transport overflow is per connection and clears on close.
- [x] **102.** **iLink loses the replacement identity when replacement quantity is at or below CumQty.** The engine emits a Canceled event for this case (`lib/src/engine/matching_engine.cpp:782`), but `lib/include/gateway/ilink/ilink_user.hpp:115` selects the original order metadata for a pending Replace and emits Cancel534 at `:133`; the reply carries the old ClOrdID and OrderRequestID rather than identifying the replacement request. Fixed in #21.
- [x] **103.** **FIX SequenceReset bypasses required timestamp validation.** `lib/include/gateway/fix/fix_session_layer.hpp:590` advances GapFill NewSeqNo and returns before the SendingTime and PossDup OrigSendingTime checks at `:605`; reset mode similarly goes straight to `handle_sequence_reset()` at `:694`, allowing malformed reset frames to change sequence state instead of being rejected. Fixed in #21.
- [x] **104.** **Configuration accepts FIX credentials the gateway cannot use.** `lib/src/server/config.cpp:384` bounds OUCH credentials but supplies no FIX length checks; FIX logon limits SenderCompID and Password to 64 characters (`lib/include/gateway/fix/fix_connection.hpp:127`), and the day state truncates configured CompIDs at `lib/include/gateway/fix/fix_session_day_state.hpp:50`, making an accepted longer username or password impossible to authenticate. Fixed in #21.
- [ ] **105.** **JSON Unicode escapes corrupt supplementary characters.** `lib/src/api/json.cpp:174` encodes each UTF-16 surrogate as a separate three-byte UTF-8 sequence instead of combining a high/low pair or rejecting an isolated surrogate; a valid escaped character such as `\uD83D\uDE00` therefore becomes invalid UTF-8 in saved configuration and subsequent JSON responses.
- [ ] **106.** **`ilink_account` is mandatory configuration with no runtime consumer.** `lib/src/server/config.cpp:392` rejects an iLink user without it, but its only production references parse, validate or declare the value, and neither the gateway nor a report reads it; occurrence counts are `lib/` 7, `src/` 0, `benchmark/` 0, `test/` 3.
- [ ] **107.** **The MDP typed decoders have only test callers.** `decode_incremental` at `lib/include/protocol/mdp3.hpp:188` has occurrence counts `lib/` 1 (its definition), `src/` 0, `benchmark/` 0, `test/` 10; `decode_snapshot` at `:209` has `lib/` 1 (its definition), `src/` 0, `benchmark/` 0, `test/` 3, so these consumer helpers belong with the test clients under the project's production-caller rule.
- [ ] **108.** **The iLink report path stages the same payload through avoidable scratch buffers.** `lib/include/protocol/ilink3.hpp:41` constructs a 2048-byte body buffer and copies the root and tail into it, then `encode_ilink()` copies that into the caller's 2048-byte buffer (`lib/include/gateway/ilink/ilink_user.hpp:262`) before the store and sink copies; writing root and tail at their final framing offsets removes one staging pass without a new abstraction.
- [ ] **109.** **Slow HTTP bodies can occupy every worker far beyond the configured timeout.** `lib/src/api/http_server.cpp:35` applies SO_RCVTIMEO per receive, while the successful-receive loop at `:230` has no request deadline; a client advertising a permitted large Content-Length can keep its slot for days by sending one byte before each idle timeout, and filling all slots prevents management requests from being served.
- [ ] **110.** **MDP publisher reconstruction tests do not cover multiple instruments.** `test/exsim/md/mdp_tests.cpp:35` adds only one instrument, and the reference consumer at `:44` maintains one bid/ask book and report counter without dispatching by SecurityID, so these tests cannot verify channel-wide event flags or independent RptSeq continuity when one batch changes multiple instruments.

- [x] **96.** Review the files the first pass did not read in full. Done, except the generated `ilink3_schema.hpp` and most test files, which were only sampled.
