# Context

What this project is and what it should show. `AGENTS.md` has the working rules.

## What it is

A small exchange simulator. A management API starts empty exchange processes; external trading engines connect to them and supply all orders.

- `exsim_api`: HTTP management API. Creates, starts, stops, and inspects exchanges.
- `exsim_exchanged`: one exchange process. Single-threaded reactor, matching engine, order entry gateways, market data.
- Order entry over TCP: OUCH 4.2 (SoupBinTCP), FIX 4.4, CME iLink 3.
- Market data over UDP: ITCH 5.0 (MoldUDP64), CME MDP 3.0.

## Layout

| Path | Holds |
|---|---|
| `lib/include/book`, `lib/src/book` | Order book storage |
| `lib/include/engine`, `lib/src/engine` | Matching engine, commands, events |
| `lib/include/protocol` | Wire codecs |
| `lib/include/gateway` | Order entry sessions per protocol |
| `lib/include/md` | Market data publishers and recovery |
| `lib/include/net`, `lib/src/net` | Sockets, poller, TCP and UDP servers |
| `lib/include/server`, `lib/src/server` | Exchange core, runtime loop, config, journal |
| `lib/include/api`, `lib/src/api` | Management service and HTTP server |
| `lib/include/util` | Hash map, object pool, SPSC queue |
| `src/` | The two binaries and tools |
| `test/` | Unit and integration tests |
| `benchmark/` | Benchmark harness and profiling scripts |

## Design goal

Simple, straightforward, lightweight. A reader should understand the whole system from the README and one pass through `lib/`. If a feature needs a long explanation, it is probably too much for this project.

## Themes the project should demonstrate

These are what a reviewer should be able to see in the code. They are the review checklist for every change.

### 1. Modern C++

- Type safety: strong types for ids, prices, quantities. `std::byte` for raw bytes. Enums with a fixed underlying type.
- Ownership is visible in the types. RAII for every resource, including file descriptors and threads.
- Use what C++23 provides (`std::variant`, `std::span`, `std::expected`, concepts, `[[likely]]`, `std::jthread`) before writing a custom version.
- When a custom structure is used instead, the reason is written down.

### 2. Linux and networking

- BSD sockets used directly: `socket`, `bind`, `listen`, `accept`, `recv`, `send`.
- An event loop on epoll (kqueue on macOS).
- Hand-written binary serialization and correct TCP semantics: partial reads and writes, framing, backpressure.
- Trade-offs are stated, for example raw TCP with epoll versus gRPC and protobuf.

### 3. Distributed systems

- Clear client and server roles. Data moves between processes and machines over defined, framed protocols.
- Sequencing, replay, and recovery are explicit.

### 4. Data structures and algorithms

- Performance is a design-time decision: preallocate, reserve, use ring buffers, keep hot data together.
- State the worst case and how each structure scales with input.
- Percentiles come from histograms.

### 5. Profiling and benchmarking

- A benchmark harness that measures the production path.
- Linux `perf` and flame graphs show where time goes.
- No performance claim without numbers from the current code.

### 6. Testing

- Unit tests for components, integration tests for sockets and processes.
- Tests mirror the `lib/` layout and run in CI with sanitizers.

### 7. README

- A new reader understands the project in 5 to 10 minutes and builds it with one command.
