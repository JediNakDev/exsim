# Tests

From the repository root:

```sh
make -j4 test
```

This runs two suites. Each is a folder, and every test file is named `*_tests.cpp`:

| Command | Folder | Coverage |
| --- | --- | --- |
| `make -j4 unit` | `exsim/` (mirrors `src/`) | Matching, books, containers, codecs, gateways, config, core policies, publishers, and ITCH replay |
| `make -j4 integration` | `integration/` | Sockets, feeds, the admin socket, and the exchange runtime, plus `integration/exsim_exchanged_test.py`, which runs the binary as a trading engine's harness would |

Shared code is not a suite: `framework/` is the test framework, `exsim/support/` holds protocol clients, the ITCH book builder, the MoldUDP64 receiver and the journal reader, and `fixtures/` holds config files. Support code links only into test binaries.

Integration tests use loopback TCP/UDP, Unix sockets and real child processes. They check startup, the ready line, runtime commands, trading, feed recovery, journals, and shutdown on signals. See [integration/net](integration/net/README.md) for the socket tests. The ITCH replay unit test compares 100,000 synthetic messages with a reference book.

CME tests cover signed iLink sessions, sequence gaps, reconnect/retransmission, order ownership, fills, cancellation, MDP book reconstruction and snapshots, and TCP packet recovery. A combined socket test checks private iLink fills against MDP trade IDs, prices, and event timestamps.

## Debug and filtering

```sh
make -j4 test MODE=debug
make -j4 unit FILTER=matching
make -j4 integration FILTER=restart
```

Debug builds use AddressSanitizer and UBSan. `FILTER` selects native cases by substring. For `integration` it is also passed to the Python test as `-k`, so a filter that matches in only one runner is fine.

On arm64 Linux (GCC 13), ASan's stack-use-after-return check makes `md_feed.clean_feed_matches_engine_after_every_batch` run for over 25 minutes instead of about 90 seconds. Run debug there with `ASAN_OPTIONS=detect_stack_use_after_return=0`.

Builds go to `build/<mode>/`. CI runs both suites on macOS and Ubuntu in release and debug modes, a ThreadSanitizer run on Ubuntu, a benchmark build, and a format check (`make format-check`, clang-format 18).

## Isolation

Each test case builds its own state and shares nothing with other cases, so the framework needs no fixtures or per-test setup. [Why not a library](../docs/architecture.md#why-not-a-library) says when to switch to one.

## Scope

Fuzz campaigns and end-to-end suites are deferred.

The active benchmark suite lives in [benchmark/](../benchmark/README.md). Replay lives in `tools/itch_replay/`. CME schema sources, regeneration, and supported scope are documented in the [CME profile](../docs/ilink-and-mdp.md).
