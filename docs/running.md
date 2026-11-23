# Running an exchange

One `exsim_exchanged` process runs one exchange. Build and start one from the repository root:

```sh
make -j4 apps
./build/release/exsim_exchanged --config test/fixtures/exchange.conf
```

`--config PATH` is the only option; everything else, including the [admin socket](#runtime-commands), is in the config file. Every start creates empty books and a fresh market data session. External trading clients provide the orders. To run several exchanges, start several processes, each with its own config.

## Ready line

Once every listener is open, the process prints one JSON line to stdout:

```json
{"state":"running","address":"127.0.0.1","orderEntryPort":15000,"fixOrderEntryPort":52114,"ilinkOrderEntryPort":0,"marketDataProtocol":"itch50_moldudp64","marketDataAddress":"239.192.10.1","marketDataPort":16000,"recoveryPort":16001,"recoveryTransport":"udp","snapshotPort":0,"instrumentReplayPort":0,"backupMarketDataPort":0,"channelId":1,"session":"2499623000"}
```

Listener port zero in the config asks the OS for a port; the ready line has the actual one. A disabled listener reports port 0. Startup errors go to stderr with exit status 1. Bad arguments exit with 2.

A test harness reads the line, trades, and stops the process:

```python
exchange = subprocess.Popen(["build/release/exsim_exchanged", "--config", "scenario.conf"],
                            stdout=subprocess.PIPE, text=True)
ports = json.loads(exchange.stdout.readline())
# connect traders to ports["orderEntryPort"] ...
exchange.terminate()
exchange.wait()
```

## Stopping

Ctrl+C (SIGINT) or `kill <pid>` (SIGTERM) shuts down cleanly: instruments close, end-of-day messages go out, sessions drain, the journal flushes, the admin socket file is removed, and the process exits with 0. `kill -9` skips all of that and leaves the socket file behind; remove it before reusing the path.

The process belongs to the shell that started it, so it ends with your SSH session. Use tmux, `nohup`, or a systemd unit to keep it running after you log out.

## Runtime commands

Name a Unix socket in the config, and the exchange takes one command per connection on it:

```ini
[exchange]
admin_socket = exsim.sock
```

A relative path is relative to the config file, like the journal. Leave the key out for no admin socket. Replies are lines ending in `OK` or `ERR <message>`:

```sh
echo 'phase AAPL continuous' | nc -U test/fixtures/exsim.sock
echo 'fault user ALICE reject 1' | nc -U test/fixtures/exsim.sock
```

A command is one line of at most 4096 bytes, ended by a newline or by closing the write side, and must arrive within 5 s of connecting. It runs on the exchange's event loop; an invalid command leaves the exchange running.

A reply ends at its first line that is `OK` or starts with `ERR`; read up to there. The exchange then closes the connection. On Linux, a client that sent more than the exchange read gets a reset there instead of end of input. A reply the client does not read within 1 s is cut off without that last line, so a stalled reader holds up trading for at most 1 s.

Send `help` for the full syntax. Commands include `status`, `book AAPL [levels]`,
`phase <SYMBOL|*> <closed|pre_open|continuous|halted|pre_close>`,
`fault user <USER> <reject|delay|drop_out|disconnect|off> [n]`, and
`fault md <drop|dup|reorder|rate|off> ...`. `shutdown` closes all instruments
and drains the process, which then exits. Phase changes can enter auctions and
uncross them on transition to continuous. Faults and phases are runtime state
and reset on restart.

The socket file is created with mode 0600, so only its owner can connect; there is no other access control. An existing path is refused. One client is served at a time and others wait. The resolved socket path is limited to 103 bytes on macOS and 107 on Linux. On Ubuntu, `nc -U` comes from `netcat-openbsd`; `socat - UNIX-CONNECT:exsim.sock` works too.

## Trading connections

Set each user's `protocol` to `ouch42`, `fix44`, or `ilink3`. All gateways share the same books. Public events use ITCH 5.0 over MoldUDP64 by default, or CME MDP 3.0 with `[market_data] protocol=mdp3`.

Configure TCP listeners with `ouch_port`, `fix_port`, and `ilink_port`. The ready line reports them as `orderEntryPort`, `fixOrderEntryPort`, and `ilinkOrderEntryPort`. iLink is disabled when no iLink users or explicit listener are configured.

MDP uses `snapshot_port` and `instrument_port` for recovery feeds and optional `backup_port` for a redundant incremental feed. `retransmit_port` is then an authenticated TCP replay listener; `recoveryTransport` reports `tcp`. ITCH recovery remains UDP. See the [CME profile, credentials, limits, and sample config](ilink-and-mdp.md).

Listeners bind `bind_address`, which traders must be able to reach. Give every feed destination a nonzero port. Running exchanges need distinct feed address and port pairs, including MDP snapshot, instrument, and backup destinations; nothing checks this across processes. A unicast feed sends to one destination address; multicast needs a working network path.

## Config files and disk state

Config files are yours to keep next to your trading engine and in version control. They hold credentials, so keep them readable by their owner only. Books and sessions are not saved.

Exchanges can use optional `[journal]` settings: `path`, `on_failure`, and `flush_interval_ms`. A relative `path` is relative to the config file. An exchange refuses a journal path whose last component is a symlink, and takes an exclusive lock on the file before truncating it, so a second running exchange that names the same journal fails to start and leaves the first one's history intact.

The legacy Exsim `fixp_sbe` protocol and `admin_port` are rejected; CME iLink 3 has its own FIXP/SBE gateway, and runtime commands use `admin_socket`.

`ilink_account` is no longer a `[user <name>]` key and nothing replaces it; the simulator never used the value. A config that still has it fails to load with `<line>: unknown key 'ilink_account'`. Remove the line.

See [architecture](architecture.md) for process boundaries.
