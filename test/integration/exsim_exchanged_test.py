"""exsim_exchanged the way a trading engine's test harness runs it.

Each test starts the binary with a config file, reads the ports from its ready
line, sends runtime commands over its admin socket and stops it with a signal.
Traders are plain TCP and UDP sockets. No external Python dependencies.
"""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import resource
import select
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / os.environ.get("EXSIM_BUILD", "build/release")
CONFIG = (ROOT / "config/exchange.conf").read_text()


def unused_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def config(feed_port=None):
    return (CONFIG.replace("ouch_port = 15000", "ouch_port = 0")
            .replace("retransmit_port = 16001", "retransmit_port = 0")
            .replace("address = 239.192.10.1", "address = 127.0.0.1")
            .replace("port = 16000", f"port = {feed_port or unused_udp_port()}"))


class Trader:
    def __init__(self, port, username, password):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.send(b"L" + username.encode().ljust(6) + password.encode().ljust(10)
                  + b" " * 10 + b"1".rjust(20))
        assert self.receive()[:1] == b"A", "login was not accepted"

    def send(self, message):
        self.sock.sendall(struct.pack("!H", len(message)) + message)

    def exact(self, size):
        result = b""
        while len(result) < size:
            part = self.sock.recv(size - len(result))
            if not part:
                raise AssertionError("truncated SoupBinTCP frame")
            result += part
        return result

    def receive(self):
        return self.exact(struct.unpack("!H", self.exact(2))[0])

    def event(self, kind):
        for _ in range(20):
            frame = self.receive()
            if frame[:2] == b"S" + kind:
                return frame[1:]
            if frame[:2] == b"SJ":
                raise AssertionError(f"order rejected: {frame!r}")
        raise AssertionError(f"missing event {kind!r}")

    def order(self, token, side, quantity=100, price=1500000, expected=b"A"):
        message = (b"O" + token.encode().ljust(14) + side + struct.pack("!I", quantity)
                   + b"AAPL    " + struct.pack("!II", price, 99999)
                   + b"TEST" + b"Y" + b"A" + b"N" + struct.pack("!I", 0) + b"N" + b" ")
        assert len(message) == 49
        self.send(b"U" + message)
        return self.event(expected)

    def close(self):
        self.sock.close()


class Exchange:
    """One exsim_exchanged child. `ready` is its ready line, or None if it exited during startup."""

    def __init__(self, directory, text, name, preexec=None):
        # The fixture's relative admin socket resolves next to this config, not the working directory.
        self.config = Path(directory) / f"{name}.conf"
        self.config.write_text(text.replace("admin_socket = exsim.sock", f"admin_socket = {name}.sock"))
        self.socket = Path(directory) / f"{name}.sock"
        self.stderr = tempfile.TemporaryFile(mode="w+")
        self.process = subprocess.Popen(
            [str(BUILD / "exsim_exchanged"), "--config", str(self.config)],
            stdout=subprocess.PIPE, stderr=self.stderr, text=True,
            # `preexec` runs in the child before exec. Exec keeps an ignored SIGHUP, so the SIGHUP
            # tests set it there and also work under nohup; every other child keeps the suite's own.
            preexec_fn=preexec)
        if not select.select([self.process.stdout], [], [], 10)[0]:
            self.process.kill()
            raise AssertionError("exchange startup timed out")
        line = self.process.stdout.readline()
        self.ready = json.loads(line) if line else None

    def admin(self, command):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(5)
            client.connect(str(self.socket))
            client.sendall(command.encode() + b"\n")
            return receive_all(client)

    def stop(self, stop_signal=signal.SIGTERM):
        if self.process.poll() is None:
            self.process.send_signal(stop_signal)
        return self.process.wait(timeout=10)

    def errors(self):
        self.stderr.seek(0)
        return self.stderr.read()

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.process.stdout.close()
        self.stderr.close()


def receive_all(client):
    reply = b""
    while chunk := client.recv(4096):
        reply += chunk
    return reply.decode()


class ExsimExchangedTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="exsim-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)

    def launch(self, text=None, name="exchange", preexec=None):
        exchange = Exchange(self.directory, text or config(), name, preexec)
        self.addCleanup(exchange.close)
        return exchange

    def start(self, text=None, name="exchange", preexec=None):
        exchange = self.launch(text, name, preexec)
        self.assertIsNotNone(exchange.ready, exchange.errors())
        return exchange

    def refused(self, text=None, name="exchange"):
        exchange = self.launch(text, name)
        self.assertIsNone(exchange.ready)
        self.assertEqual(exchange.process.wait(timeout=10), 1)
        return exchange.errors()

    def test_ready_line_reports_ports_and_the_admin_socket_is_owner_only(self):
        exchange = self.start()
        self.assertEqual(exchange.ready["state"], "running")
        self.assertGreater(exchange.ready["orderEntryPort"], 0)
        self.assertGreater(exchange.ready["recoveryPort"], 0)
        self.assertEqual(exchange.ready["ilinkOrderEntryPort"], 0)
        mode = exchange.socket.stat().st_mode
        self.assertTrue(stat.S_ISSOCK(mode))
        self.assertEqual(stat.S_IMODE(mode), 0o600)
        status = exchange.admin("status")
        self.assertIn("orders live=0", status)
        self.assertTrue(status.endswith("OK\n"), status)

    def test_admin_changes_phase_and_injects_user_fault(self):
        exchange = self.start()
        trader = Trader(exchange.ready["orderEntryPort"], "ALICE", "alice")
        self.addCleanup(trader.close)
        self.assertIn("phase", exchange.admin("help"))
        self.assertTrue(exchange.admin("unknown").startswith("ERR"))
        self.assertIn("ERR", exchange.admin("phase UNKNOWN halted"))
        self.assertEqual(exchange.admin("x" * 4097), "ERR command longer than 4096 bytes\n")
        # The exchange rejects the line at 4608 bytes; it discards the rest so Linux does not reset.
        self.assertEqual(exchange.admin("x" * 6000), "ERR command longer than 4096 bytes\n")
        self.assertIn("HALTED", exchange.admin("phase AAPL halted"))
        self.assertIn("phase=HALTED", exchange.admin("book AAPL"))
        trader.order("HALTED", b"B", expected=b"J")
        self.assertIn("orders live=0", exchange.admin("status"))
        self.assertIn("CONTINUOUS", exchange.admin("phase AAPL continuous"))
        self.assertIn("reject=1", exchange.admin("fault user ALICE reject 1"))
        trader.order("FAULT", b"B", expected=b"J")
        trader.order("ACCEPTED", b"B")
        self.assertIn("orders live=1", exchange.admin("status"))
        self.assertIn("OK", exchange.admin("fault md drop 1"))
        self.assertIn("OK", exchange.admin("fault md off"))
        self.assertIn("OK", exchange.admin("shutdown"))
        self.assertEqual(exchange.process.wait(timeout=10), 0)
        self.assertFalse(exchange.socket.exists())

    def test_admin_answers_one_line_per_connection_however_it_arrives(self):
        instrument = CONFIG[CONFIG.index("[instrument AAPL]"):CONFIG.index("[user")].replace("AAPL", "ERR")
        exchange = self.start(config() + "\n" + instrument)
        # A body line may start with ERR; only end of input ends the reply.
        self.assertEqual(exchange.admin("phase ERR halted"), "ERR CONTINUOUS -> HALTED\nOK\n")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(5)
            client.connect(str(exchange.socket))
            client.sendall(b"sta")
            time.sleep(0.05)
            client.sendall(b"tus\r\n")
            self.assertIn("orders live=0", receive_all(client))
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(5)
            client.connect(str(exchange.socket))
            client.sendall(b"help\nshutdown\n")
            self.assertIn("phase", receive_all(client))
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(5)
            client.connect(str(exchange.socket))
            client.sendall(b"status")
            client.shutdown(socket.SHUT_WR)
            self.assertIn("orders live=0", receive_all(client))
        self.assertIsNone(exchange.process.poll())

    def test_int_term_and_hup_stop_cleanly_and_remove_the_admin_socket(self):
        for stop_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            exchange = self.start(preexec=lambda: signal.signal(signal.SIGHUP, signal.SIG_DFL))
            self.assertTrue(exchange.socket.exists())
            self.assertEqual(exchange.stop(stop_signal), 0)
            self.assertFalse(exchange.socket.exists())

    def test_hup_inherited_as_ignored_keeps_running_like_nohup(self):
        exchange = self.start(preexec=lambda: signal.signal(signal.SIGHUP, signal.SIG_IGN))
        exchange.process.send_signal(signal.SIGHUP)
        # A loop that took the SIGHUP stops on its next pass, so it answers at most one command.
        for _ in range(2):
            self.assertIn("orders live=0", exchange.admin("status"))
        self.assertIsNone(exchange.process.poll())
        self.assertEqual(exchange.stop(), 0)

    def test_bad_arguments_and_configs_exit_nonzero(self):
        binary = str(BUILD / "exsim_exchanged")
        for arguments in ([], ["--admin-socket", "x.sock"], ["--config"], ["--config", "x.conf", "--bogus"]):
            self.assertEqual(subprocess.run([binary, *arguments], capture_output=True, timeout=5).returncode, 2)
        self.assertIn("invalid exchange config", self.refused("broken"))
        for extra in ("\n[bot maker]\ntype=market_maker\n", "\n[exchange]\nfixp_sbe_port=15001\n"):
            self.refused(config() + extra)

    def test_existing_admin_socket_path_is_refused_and_left_alone(self):
        first = self.start()
        self.assertIn("admin socket", self.refused(name="exchange"))
        self.assertIn("orders live=0", first.admin("status"))

    def test_a_refused_restart_keeps_the_last_journal(self):
        text = config() + "\n[journal]\npath = run.journal\n"
        last = self.start(text)
        self.assertIn("HALTED", last.admin("phase AAPL halted"))
        self.assertEqual(last.stop(), 0)
        journal = (self.directory / "run.journal").read_bytes()
        with socket.socket() as busy:
            busy.bind(("127.0.0.1", 0))
            busy.listen()
            busy_port = text.replace("ouch_port = 0", f"ouch_port = {busy.getsockname()[1]}")
            self.assertIn("listener", self.refused(busy_port, name="busy"))
        self.assertEqual((self.directory / "run.journal").read_bytes(), journal)
        stale = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        stale.bind(str(last.socket))  # the file a kill -9 leaves behind
        stale.close()
        self.assertIn("admin socket", self.refused(text))
        self.assertEqual((self.directory / "run.journal").read_bytes(), journal)

    def test_port_conflict_exits_with_the_listener_error(self):
        with socket.socket() as busy:
            busy.bind(("127.0.0.1", 0))
            busy.listen()
            text = config().replace("ouch_port = 0", f"ouch_port = {busy.getsockname()[1]}")
            self.assertIn("listener", self.refused(text))

    def test_external_sessions_match_and_publish_recoverable_market_data(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as feed:
            feed.bind(("127.0.0.1", 0))
            feed.settimeout(3)
            exchange = self.start(config(feed.getsockname()[1]))
            port = exchange.ready["orderEntryPort"]
            alice, bob = Trader(port, "ALICE", "alice"), Trader(port, "BOB", "bob")
            self.addCleanup(alice.close)
            self.addCleanup(bob.close)
            alice.order("BUY", b"B")
            bob.order("SELL", b"S")
            self.assertEqual(alice.event(b"E")[:1], b"E")
            self.assertEqual(bob.event(b"E")[:1], b"E")
            self.assertIn("orders live=0", exchange.admin("status"))
            packet, _ = feed.recvfrom(65535)
            session, sequence, count = struct.unpack("!10sQH", packet[:20])
            self.assertEqual(session.decode(), exchange.ready["session"])
            self.assertGreater(count, 0)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as recovery:
                recovery.settimeout(3)
                recovery.sendto(struct.pack("!10sQH", session, sequence, count),
                                ("127.0.0.1", exchange.ready["recoveryPort"]))
                replay, _ = recovery.recvfrom(65535)
            self.assertEqual(replay, packet)

    def test_journal_next_to_the_config_records_external_orders_and_shutdown(self):
        exchange = self.start(config() + "\n[journal]\npath = run.journal\n")
        trader = Trader(exchange.ready["orderEntryPort"], "ALICE", "alice")
        self.addCleanup(trader.close)
        trader.order("JOURNALED", b"B")
        self.assertIn("journal state=ok", exchange.admin("status"))
        self.assertEqual(exchange.stop(), 0)
        data = (self.directory / "run.journal").read_bytes()
        self.assertEqual(struct.unpack("!QQ", data[:16]), (0x455853494D4A4E4C, 3))
        count = struct.unpack("!Q", data[40:48])[0]
        offset = 56 + 108 * count
        tags = []
        while offset < len(data) - 24:
            sequence, size, tag = struct.unpack("!QQQ", data[offset:offset + 24])
            self.assertEqual(sequence, len(tags))
            self.assertLessEqual(size + 24, 128)
            tags.append(tag)
            offset += size + 24
        self.assertEqual(offset, len(data) - 24)
        magic, records, _ = struct.unpack("!QQQ", data[-24:])
        self.assertEqual(magic, 0x455853494D454E44)
        self.assertEqual(records, len(tags))
        self.assertGreaterEqual(records, 2)
        self.assertEqual(tags[0], 0)  # NewOrder from the external client
        self.assertEqual(tags[-1], 4)  # SetPhase Closed at shutdown

    def test_symlinked_journal_is_refused_and_its_target_is_unchanged(self):
        outside = self.directory / "outside"
        outside.write_text("sentinel")
        (self.directory / "linked.journal").symlink_to(outside)
        self.refused(config() + "\n[journal]\npath = linked.journal\n")
        self.assertEqual(outside.read_text(), "sentinel")

    def test_a_failed_journal_write_exits_with_1_and_says_so(self):
        def small_files():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)  # a write past the limit fails with EFBIG
            resource.setrlimit(resource.RLIMIT_FSIZE, (512, 512))  # the header fits, the records do not

        text = config() + "\n[journal]\npath = full.journal\nflush_interval_ms = 0\n"
        exchange = self.start(text, preexec=small_files)
        trader = Trader(exchange.ready["orderEntryPort"], "ALICE", "alice")
        self.addCleanup(trader.close)
        for number in range(10):
            trader.order(f"FULL{number}", b"B")
        self.assertEqual(exchange.stop(), 1)
        journal = self.directory / "full.journal"
        self.assertIn(f"exsim_exchanged: journal {journal} failed; it is incomplete", exchange.errors())

    def test_shared_journal_allows_one_running_exchange_and_keeps_its_history(self):
        journal = self.directory / "shared.journal"
        first = self.start(config() + "\n[journal]\npath = shared.journal\n", name="first")
        header = journal.read_bytes()
        # A different engine size gives a different header, so a truncating second writer is visible.
        # The first exchange may append a flush footer meanwhile, so compare the prefix.
        self.refused(config().replace("max_orders = 4096", "max_orders = 2048")
                     + "\n[journal]\npath = shared.journal\n", name="second")
        self.assertTrue(journal.read_bytes().startswith(header))
        self.assertEqual(first.stop(), 0)
        self.start(config() + "\n[journal]\npath = shared.journal\n", name="restarted")

    def test_restart_is_empty_with_a_fresh_session(self):
        text = config()
        first = self.start(text, name="first")
        trader = Trader(first.ready["orderEntryPort"], "ALICE", "alice")
        trader.order("BEFORE", b"B")
        self.assertEqual(first.stop(), 0)
        trader.close()
        second = self.start(text, name="second")
        self.assertIn("orders live=0", second.admin("status"))
        self.assertNotEqual(second.ready["session"], first.ready["session"])

    def test_fix_sessions_trade_through_the_reported_port(self):
        exchange = self.start(config().replace("protocol = ouch42", "protocol = fix44"))
        port = exchange.ready["fixOrderEntryPort"]
        self.assertGreater(port, 0)
        self.assertNotEqual(port, exchange.ready["orderEntryPort"])

        def receive(sock):
            # Read one FIX frame by BodyLength, preserving any following frame.
            header = b""
            while header.count(b"\x01") < 2:
                byte = sock.recv(1)
                self.assertTrue(byte, "FIX connection closed before header")
                header += byte
            body_length = int(header.split(b"\x01")[1].split(b"=")[1])
            remaining = body_length + 7  # Body plus 10=ddd<SOH>.
            body = b""
            while len(body) < remaining:
                chunk = sock.recv(remaining - len(body))
                self.assertTrue(chunk, "FIX connection closed before checksum")
                body += chunk
            return header + body

        def send(sock, user, kind, sequence, fields):
            timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
            body = f"35={kind}|49={user}|56=EXSIM|34={sequence}|52={timestamp}|{fields}".replace("|", "\x01").encode()
            frame = f"8=FIX.4.4\x019={len(body)}\x01".encode() + body
            sock.sendall(frame + f"10={sum(frame) % 256:03d}\x01".encode())

        alice = socket.create_connection(("127.0.0.1", port), timeout=3)
        bob = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.addCleanup(alice.close)
        self.addCleanup(bob.close)
        send(alice, "ALICE", "A", 1, "98=0|108=30|554=alice|")
        self.assertIn(b"35=A\x01", receive(alice))
        send(bob, "BOB", "A", 1, "98=0|108=30|554=bob|")
        self.assertIn(b"35=A\x01", receive(bob))
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
        send(alice, "ALICE", "D", 2, f"11=BUY|55=AAPL|54=1|38=100|40=2|44=150|59=0|60={timestamp}|")
        self.assertIn(b"39=0\x01", receive(alice))
        self.assertIn("orders live=1", exchange.admin("status"))
        send(bob, "BOB", "D", 2, f"11=SELL|55=AAPL|54=2|38=100|40=2|44=150|59=0|60={timestamp}|")
        for sock in (bob, alice):
            for _ in range(3):
                if b"39=2\x01" in receive(sock):
                    break
            else:
                self.fail("FIX execution report did not reach the participant")
        self.assertIn("orders live=0", exchange.admin("status"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
