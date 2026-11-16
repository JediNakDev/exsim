# Tests

From the repository root:

```sh
make -j4 test
```

This runs two suites. Each is a folder, and every test file is named `*_tests.cpp`:

| Command | Folder | Coverage |
| --- | --- | --- |
| `make -j4 unit` | `exsim/` (mirrors `lib/`) | Matching, books, containers, codecs, gateways, config, JSON, core policies, publishers, and ITCH replay |
| `make -j4 integration` | `integration/` | Sockets, HTTP, feeds, and API-managed exchange processes, plus the Python API test `integration/exchange_api_test.py` |

Shared code is not a suite: `framework/` is the test framework, `exsim/support/` holds protocol clients, the ITCH book builder, the MoldUDP64 receiver and the journal reader, and `fixtures/` holds config files. Support code links only into test binaries.

Integration tests use loopback TCP/UDP and real child processes. They check lifecycle, trading, feed recovery, conflicts, isolation, and shutdown. See [integration/net](integration/net/README.md) for the socket tests. The ITCH replay unit test compares 100,000 synthetic messages with a reference book.

CME tests cover signed iLink sessions, sequence gaps, reconnect/retransmission, order ownership, fills, cancellation, MDP book reconstruction and snapshots, TCP packet recovery, and API-managed processes. A combined socket test checks private iLink fills against MDP trade IDs, prices, and event timestamps.

## Debug and filtering

```sh
make -j4 test MODE=debug
make -j4 unit FILTER=matching
make -j4 integration FILTER=restart
```

Debug builds use AddressSanitizer and UBSan. `FILTER` selects native cases by substring. For `integration` it is also passed to the Python test as `-k`, so a filter that matches in only one runner is fine.

Builds go to `build/<mode>/`. CI runs both suites on macOS and Ubuntu in release and debug modes, a ThreadSanitizer run on Ubuntu, a benchmark build, and a format check (`make format-check`, clang-format 18).

## Isolation

Each test case builds its own state and shares nothing with other cases, so the framework needs no fixtures or per-test setup. [Why not a library](../docs/architecture.md#why-not-a-library) says when to switch to one.

## Scope

Fuzz campaigns and end-to-end suites are deferred.

The active benchmark suite lives in [benchmark/](../benchmark/README.md). Replay lives in `src/utils/itch_replay/`. CME schema sources, regeneration, and supported scope are documented in the [CME profile](../docs/ilink-and-mdp.md).
