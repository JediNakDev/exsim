# Tests

From the repository root:

```sh
make -j4 test
```

This runs two suites. Each is a folder, and every test file is named `*_tests.cpp`:

| Command | Folder | Coverage |
| --- | --- | --- |
| `make -j4 unit` | `exsim/` (mirrors `src/`) | Matching, books, containers, codecs, gateways, config, core policies, publishers, and ITCH replay |
| `make -j4 integration` | `integration/` | Sockets, feeds, the admin socket, and the exchange runtime, plus `integration/exsim_exchanged_test.py`, which runs the binary as a trading engine's harness would, and `integration/wire_bench_test.py`, which checks that the wire benchmark fails on a wrong reply and stops its exchange on a signal |

Shared code is not a suite: `framework/` is the test framework, `exsim/support/` holds protocol clients, the ITCH book builder and the MoldUDP64 receiver, and `fixtures/` holds config files. Support code links only into test binaries.

Integration tests use loopback TCP/UDP, Unix sockets and real child processes. They check startup, the ready line, runtime commands, trading, feed recovery, journals and their replay, and shutdown on signals. See [integration/net](integration/net/README.md) for the socket tests. The ITCH replay unit test compares 100,000 synthetic messages with a reference book.

CME tests cover signed iLink sessions, sequence gaps, reconnect/retransmission, order ownership, fills, cancellation, MDP book reconstruction and snapshots, and TCP packet recovery. A combined socket test checks private iLink fills against MDP trade IDs, prices, and event timestamps.

## Debug and filtering

```sh
make -j4 test MODE=debug
make -j4 unit FILTER=matching
make -j4 integration FILTER=restart
```

Debug builds use AddressSanitizer and UBSan. `FILTER` selects native cases by substring. For `integration` it is also passed to the Python test as `-k`, so a filter that matches in only one runner is fine.

On arm64, GCC 12, 13.1 to 13.3.0 and 14.1 to 14.2.0 can leave ASan fake stack frames marked in use after a function returns ([fixed upstream](https://github.com/gcc-mirror/gcc/commit/13d971601175541146fda54682cde4d3f9cd2759) in 13.4, 14.3 and 15). Once a thread has no free frame of one size, after thousands of calls, each call needing that size scans the full table (about 7 us) and the stack-use-after-return check misses bugs in those frames. That stretched the arm64 Linux debug unit suite from about 3 minutes to over 9 hours, so the Makefile turns the check off for those releases on arm64. Short runs, such as the integration tests' child processes, lose the check while it would still work. x86-64 builds, as in CI, fixed releases, and 13.3.1 or 14.2.1 snapshots (which may hold the fix) keep it. With ASan's default fake stack size, `test_framework.asan_fake_stack_does_not_fill_up` fails if a build runs the broken check.

Builds go to `build/<mode>/`. CI runs both suites on macOS and Ubuntu in release and debug modes, a ThreadSanitizer run on Ubuntu, a benchmark build with a short wire benchmark run, a fuzz run (45 seconds per harness), and a format check (`make format-check`, clang-format 18).

## Fuzzing

```sh
make fuzz
make fuzz FILTER=gateway FUZZ_SECONDS=600
```

`test/fuzz/` holds one libFuzzer harness per parser of untrusted input: the FIX 4.4, SoupBinTCP/OUCH 4.2 and iLink 3 order-entry connections, the MoldUDP64 retransmission request, the MDP TCP replay request, the config file and the admin command line. Each passes its bytes to the production code, built with ASan and UBSan. The gateway and admin harnesses build a full exchange core per input (`test/fuzz/exchange.hpp`).

Each harness runs for `FUZZ_SECONDS` (default 60) and stops at the first crash or sanitizer report, leaving the input in `build/fuzz/`. Replay it with `./build/fuzz/fuzz_<name> <file>`, and give the fix a unit test that replays the same input. Seeds come from `test/fuzz/seeds/<name>` and, for the config harness, `config/examples/*.conf`. New inputs collect in `build/fuzz/corpus/<name>`.

libFuzzer ships with LLVM clang, not Apple clang, so run it on Linux. On Ubuntu 24.04, install `clang-19 libclang-rt-19-dev`. Set `FUZZ_CXX` to use another clang, version 19 or later.

## FIX interop check

`interop/fix44_quickfix.py` runs a QuickFIX 1.16 initiator (Python bindings) against `exsim_exchanged` started from `interop/fix44_quickfix.conf`. QuickFIX validates every message against its own FIX44.xml. The check covers a refused Logon, Logon, orders, cancels and replaces, fills across two sessions, Heartbeat and TestRequest, ResendRequest in both directions, and Logout. It is optional and not in CI, because building QuickFIX takes a few minutes. On Ubuntu:

```sh
sudo apt install python3-venv python3-dev g++
python3 -m venv output/quickfix
output/quickfix/bin/pip install quickfix==1.16.0
make -j4 apps
output/quickfix/bin/python test/interop/fix44_quickfix.py
```

It prints PASS or FAIL and the folder with the QuickFIX logs. `EXSIM_BUILD` selects the build folder (default `build/release`).

## Isolation

Each test case builds its own state and shares nothing with other cases, so the framework needs no fixtures or per-test setup. [Why not a library](../docs/architecture.md#why-not-a-library) says when to switch to one.

## Scope

End-to-end suites are deferred.

The active benchmark suite lives in [benchmark/](../benchmark/README.md). ITCH replay lives in `benchmark/itch_replay/`. CME schema sources, regeneration, and supported scope are documented in the [CME profile](../docs/ilink-and-mdp.md).
