# Exsim

A simple exchange simulator for testing trading strategies with various trading protocal supported.

```mermaid
flowchart TD
  subgraph SERVER
    EX1[Exchange Process]
  end
  EX1 <-->|"TCP: Orders and private replies"| TD1[Trader]
  EX1 <-->|"UDP: Market data and recovery"| TD1
```

## Getting Started

### Building

From the repository root:

```sh
make -j4 apps
```

### Running

```sh
./build/release/exsim_exchanged --config config/exchange.conf
```

This builds the exchange and starts one with empty books, waiting for external trading clients. Once ready it prints one JSON line with its ports. Each exchange is its own process: start more with their own config files, and stop one with **Ctrl+C** or `kill <pid>`.

[config/exchange.conf](./config/exchange.conf) is a commented example of every setting; copy it to start your own exchange. [config/cme.conf](./config/cme.conf) is the CME profile, with MDP 3.0 market data and iLink 3 order entry.

See [Running an exchange](./docs/running.md) for the ready line, runtime commands, and config.

### Test and Benchmark

```sh
make -j4 test
make -j4 bench
```

## Limitations

- This is a very early alpha, expects bugs.
- We provided only exchange simulator, you have to have your own trader engine.
- Currently support ITCH 5.0, OUCH 4.2, FIX 4.4, iLink 3, and MDP 3.0.

## Further Reading

- [Running an exchange](./docs/running.md)
- [Architecture](./docs/architecture.md)
- [Choices of Data structures and Rationale](./docs/dsa.md)
- [TEST](./test/README.md)
- [BENCHMARK](./benchmark/README.md)
