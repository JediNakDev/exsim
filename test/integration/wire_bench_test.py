"""exsim_wire_bench against the real exchange and against a scripted one.

The scripted exchange answers the benchmark the way exsim_exchanged does, but
can break one rule. The benchmark must then fail rather than report numbers.
Each run gets its own folder (so its own wire_latency.conf) and its own process
group, which cleanup kills. No external Python dependencies.
"""
import os
from pathlib import Path
import pty
import signal
import subprocess
import sys
import tempfile
import termios
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / os.environ.get("EXSIM_BUILD", "build/release")

# Runs as exsim_exchanged next to a symlink of the benchmark. Orders rest, and a
# buy fills the oldest resting sell, as they do in the benchmark's script. It
# writes exchange.pid at start, `marker` once the benchmark waits on it, and on
# SIGTERM a copy of the benchmark's CSV as it is then, `csv_at_sigterm`.
SCRIPTED_EXCHANGE = r'''
import os, re, signal, socket, struct, sys, time
fault = os.environ["WIRE_FAULT"]
folder = os.path.dirname(sys.argv[2])
feed_port = int(re.search(r"^port = (\d+)$", open(sys.argv[2]).read(), re.M).group(1))

def mark():
    open(os.path.join(folder, "marker"), "w").close()

def on_sigterm(*_):
    with open(os.path.join(folder, "wire_results.csv")) as csv:
        open(os.path.join(folder, "csv_at_sigterm"), "w").write(csv.read())
    if fault == "ignore_sigterm":
        mark()  # the benchmark is in its shutdown wait
        return
    if fault == "sigterm_the_benchmark":
        os.kill(os.getppid(), signal.SIGTERM)
    os._exit(0)

signal.signal(signal.SIGTERM, on_sigterm)
open(os.path.join(folder, "exchange.pid"), "w").write(str(os.getpid()))
listener = socket.create_server(("127.0.0.1", 0))
session = b"SCRIPTED  "
print('{"orderEntryPort":%d,"session":"%s"}' % (listener.getsockname()[1], session.decode()), flush=True)
connection, _ = listener.accept()
feed = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sequence = 1

def read(size):
    data = b""
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            # The benchmark is done; wait for its SIGTERM. Not signal.pause(): a signal that lands
            # just before pause() blocks leaves the handler pending and pause() waiting for good.
            time.sleep(0.01)
        data += part
    return data

def reply(kind, payload):
    connection.sendall(struct.pack("!H", len(payload) + 1) + kind + payload)

def publish(message):
    global sequence
    header = (b"OTHER     " if fault == "session" else session) + struct.pack("!QH", sequence, 1)
    extra = b"!" if fault == "trailing" else b""
    feed.sendto(header + struct.pack("!H", len(message)) + message + extra, ("127.0.0.1", feed_port))
    sequence += 1

read(49)
if fault == "hang":
    mark()  # the benchmark is waiting for its login reply
    while True:
        signal.pause()
reply(b"A", session + b"1".rjust(20))
reference, sells, crosses, last_cross_ack = 0, [], 0, b""
while True:
    order = read(52)[3:]
    reference += 1
    token, side, shares, price = order[1:15], order[15:16], order[16:20], order[28:32]
    if fault == "leading_zero":
        token = b"0" + token[:-1]  # "01" for token "1"
    ack = (b"A" + bytes(8) + token + side + shares + order[20:28] + price + order[32:40] + b"Y"
           + struct.pack("!Q", reference) + b"AN" + bytes(4) + b"NL ")
    if side == b"B" and sells:
        crosses += 1
        if fault == "duplicate_ack" and crosses % 2 == 0:
            ack = last_cross_ack  # repeats the previous buy's ack instead of this one's
        last_cross_ack = ack
        reply(b"S", ack)
        filled = 1 if fault == "partial_fill" else 100
        publish(b"E" + bytes(10) + struct.pack("!QIQ", sells.pop(0), filled, reference))
    else:
        reply(b"S", ack)
        if side == b"S":
            sells.append(reference)
        publish(b"A" + bytes(10) + struct.pack("!Q", reference) + side + shares + order[20:28] + price)
'''


def fill(descriptor):
    """Writes to a pipe or terminal until it takes no more, so a later write blocks while no one reads."""
    os.set_blocking(descriptor, False)
    for size in (4096, 1):
        try:
            while True:
                os.write(descriptor, bytes(size))
        except BlockingIOError:
            pass
    os.set_blocking(descriptor, True)


def kill_group(bench):
    """Kills what the benchmark left running. Only while it is unreaped: until then
    its pid, which is the group id, cannot be reused by another process."""
    if bench.returncode is None:
        try:
            os.killpg(bench.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass  # nothing in the group is alive (macOS reports EPERM for a group of zombies)
    bench.communicate(timeout=30)


def wait_for(condition, what):
    deadline = time.monotonic() + 10
    while not condition():
        if time.monotonic() > deadline:
            raise AssertionError(f"no {what} within 10 s")
        time.sleep(0.01)


def gone(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    return False


class WireBenchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="exsim-wire-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)

    def start(self, fault, *args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, preexec_fn=None):
        """The benchmark in a new folder, next to the real exchange (fault None)
        or the scripted one, in a new process group. Returns it and the folder."""
        folder = Path(tempfile.mkdtemp(dir=self.directory))
        (folder / "exsim_wire_bench").symlink_to((BUILD / "exsim_wire_bench").resolve())
        exchange = folder / "exsim_exchanged"
        if fault is None:
            exchange.symlink_to((BUILD / "exsim_exchanged").resolve())
        else:
            exchange.write_text(f"#!{sys.executable}\n{SCRIPTED_EXCHANGE}")
            exchange.chmod(0o755)
        # MallocNanoZone=0: an ASan build on macOS warns that it abandoned the nano zone before main, on
        # stderr when that is a terminal, else through a log socket that takes the lowest free descriptor.
        bench = subprocess.Popen([str(folder / "exsim_wire_bench"), *args],
                                 env={**os.environ, "WIRE_FAULT": fault or "", "MallocNanoZone": "0"},
                                 stdout=stdout, stderr=stderr,
                                 text=True, start_new_session=True, preexec_fn=preexec_fn, cwd=folder)
        self.addCleanup(kill_group, bench)
        return bench, folder

    def finish(self, bench, timeout=30):
        try:
            _, errors = bench.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail("the benchmark or a process it started is still running")
        return bench.returncode, errors

    def test_reports_every_pass_against_the_real_exchange(self):
        bench, folder = self.start(None, "--orders=100", "--window=8")
        report, errors = bench.communicate(timeout=120)
        self.assertEqual(bench.returncode, 0, errors)
        # The benchmark is a symlink in its own folder: the report names the exchange it started there.
        self.assertEqual(report.splitlines()[-2:],
                         [f"Exchange: {folder / 'exsim_exchanged'}", f"Results: {folder / 'wire_results.csv'}"])
        rows =[line.split(",") for line in (folder / "wire_results.csv").read_text().splitlines()[1:]]
        self.assertEqual([row[0] for row in rows],
                         [f"{workload}_{metric}_w{window}" for window in (1, 8)
                          for workload in ("rest", "cross") for metric in ("ack", "feed")])
        self.assertTrue(all(row[1] == "100" for row in rows))

    def test_scripted_exchange_without_a_fault_passes(self):
        bench, folder = self.start("none", "--orders=2", "--window=2")
        self.assertEqual(self.finish(bench), (0, ""))
        # Only the header while the exchange runs, so a CSV reader that stops reading cannot keep it running.
        self.assertEqual(len((folder / "csv_at_sigterm").read_text().splitlines()), 1)

    def test_reports_a_client_core_only_on_linux(self):
        # macOS has no core pinning, only an affinity hint, so its table must not name a core. The label says what
        # --core did, not whether the client is pinned: an affinity from the launcher (taskset) is not visible to it.
        core = min(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else 0
        bench, _ = self.start("none", "--orders=2", "--window=2", f"--core={core}")
        report, errors = bench.communicate(timeout=30)
        self.assertEqual(bench.returncode, 0, errors)
        pinned = str(core) if sys.platform == "linux" else "no"
        self.assertEqual(report.splitlines()[0], f"Orders per pass: 2; client pinned by --core: {pinned}")

    def test_fails_on_a_wrong_reply(self):
        for fault, message in [("duplicate_ack", "Order Accepted out of order"),
                               ("leading_zero", "Order Accepted out of order"),
                               ("partial_fill", "not a full fill"),
                               ("session", "another MoldUDP64 session"),
                               ("trailing", "after the last MoldUDP64 message")]:
            with self.subTest(fault):
                bench, _ = self.start(fault, "--orders=2", "--window=2")
                code, errors = self.finish(bench)
                self.assertEqual(code, 1)
                self.assertIn(message, errors)

    def assert_signal_stops_the_exchange_too(self, fault, number, preexec_fn=None):
        bench, folder = self.start(fault, "--orders=2", "--window=2", preexec_fn=preexec_fn)
        wait_for(lambda: (folder / "marker").exists(), "marker from the scripted exchange")
        exchange = int((folder / "exchange.pid").read_text())
        os.kill(bench.pid, number)
        self.assertEqual(self.finish(bench), (-number, ""))  # ended by the same signal, with no message
        wait_for(lambda: gone(exchange), "exit of the exchange")

    def test_a_signal_while_waiting_for_login_stops_the_exchange_too(self):
        for number in (signal.SIGHUP, signal.SIGINT, signal.SIGPIPE, signal.SIGTERM):
            with self.subTest(signal.Signals(number).name):
                self.assert_signal_stops_the_exchange_too("hang", number)
        with self.subTest("SIGHUP under nohup"):  # which ignores it in the process it starts
            nohup = lambda: signal.signal(signal.SIGHUP, signal.SIG_IGN)
            self.assert_signal_stops_the_exchange_too("hang", signal.SIGHUP, nohup)

    def test_a_signal_while_the_exchange_ignores_sigterm_stops_it_too(self):
        self.assert_signal_stops_the_exchange_too("ignore_sigterm", signal.SIGTERM)

    def test_a_signal_while_the_exchange_exits_ends_the_run_with_no_report(self):
        report = self.directory / "report"
        with report.open("w") as stdout:
            bench, _ = self.start("sigterm_the_benchmark", "--orders=2", "--window=2", stdout=stdout)
        self.assertEqual(self.finish(bench), (-signal.SIGTERM, ""))
        self.assertEqual(report.read_text(), "")

    def test_bad_arguments_are_a_usage_error(self):
        for argument in ("--orders=3oops", "--orders=18446744073709551615"):
            with self.subTest(argument):
                bench, _ = self.start("none", argument)
                self.assertEqual(self.finish(bench)[0], 2)

    def test_refuses_a_name_found_on_path(self):
        # With no folder in argv[0] the client would start whichever exchange is in the current folder.
        elsewhere = Path(tempfile.mkdtemp(dir=self.directory))
        bench = subprocess.Popen(["exsim_wire_bench", "--orders=2"], executable=(BUILD / "exsim_wire_bench").resolve(),
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True,
                                 cwd=elsewhere)
        self.addCleanup(kill_group, bench)
        code, errors = self.finish(bench)
        self.assertEqual((code, errors), (2, "exsim_wire_bench: run it by its path, as build/bench/exsim_wire_bench\n"))
        self.assertEqual(list(elsewhere.iterdir()), [])

    def test_starts_the_exchange_beside_the_client_its_path_reaches(self):
        # "beside/link/.." reaches the folder that holds the link's target, not "beside", whose decoy fails the run.
        reached, beside = Path(tempfile.mkdtemp(dir=self.directory)), Path(tempfile.mkdtemp(dir=self.directory))
        (reached / "inner").mkdir()
        for name in ("exsim_wire_bench", "exsim_exchanged"):
            (reached / name).symlink_to((BUILD / name).resolve())
        (beside / "link").symlink_to(reached / "inner")
        (beside / "exsim_exchanged").write_text("#!/bin/sh\nexit 3\n")
        (beside / "exsim_exchanged").chmod(0o755)
        bench = subprocess.Popen([str(beside / "link" / ".." / "exsim_wire_bench"), "--orders=2", "--window=1"],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True,
                                 cwd=reached)
        self.addCleanup(kill_group, bench)
        report, errors = bench.communicate(timeout=30)
        self.assertEqual(bench.returncode, 0, errors)
        self.assertEqual(report.splitlines()[-2], f"Exchange: {beside / 'link' / '..' / 'exsim_exchanged'}")
        self.assertEqual(sorted(path.name for path in beside.iterdir()), ["exsim_exchanged", "link"])

    def test_fails_when_stdout_is_closed(self):
        read_end, pipe = os.pipe()
        os.close(read_end)
        self.addCleanup(os.close, pipe)
        controller, terminal = pty.openpty()
        os.close(controller)
        self.addCleanup(os.close, terminal)
        # The report goes out in one write once the exchange has stopped. A closed pipe then ends the
        # run by SIGPIPE, as it would any program, and a terminal whose controller is closed fails it with EIO.
        message = "exsim_wire_bench: cannot write the report to stdout\n"
        for name, stdout, expected in [("pipe", pipe, (-signal.SIGPIPE, "")), ("terminal", terminal, (1, message))]:
            with self.subTest(name):
                bench, _ = self.start("none", "--orders=2", "--window=2", stdout=stdout)
                self.assertEqual(self.finish(bench), expected)

    def test_one_signal_ends_a_run_whose_output_is_never_read(self):
        read_end, pipe = os.pipe()
        controller, terminal = pty.openpty()
        # The controller closes first, so closing the terminal does not wait for its output to drain.
        for descriptor in (read_end, pipe, terminal, controller):
            self.addCleanup(os.close, descriptor)
        # Terminal output stopped, as after Ctrl-S. Linux would otherwise make room on a full terminal
        # some time after the fill, as it moves queued output on to the controller.
        termios.tcflow(terminal, termios.TCOOFF)
        for name, output in [("pipe", pipe), ("terminal", terminal)]:
            with self.subTest(name):
                fill(output)
                # stdout and stderr share it, as with 2>&1, so an error message would block too.
                bench, folder = self.start("none", "--orders=2", "--window=2", stdout=output, stderr=output)
                pid_file = folder / "exchange.pid"
                wait_for(lambda: pid_file.exists() and pid_file.read_text() != "", "exchange.pid")
                exchange = int(pid_file.read_text())
                wait_for(lambda: gone(exchange), "exit of the exchange")  # nothing is written before
                os.kill(bench.pid, signal.SIGTERM)  # in the report write, or just before it
                self.assertEqual(self.finish(bench, timeout=10)[0], -signal.SIGTERM)

    def test_one_signal_ends_a_run_whose_csv_is_never_read(self):
        read_end, pipe = os.pipe()
        for descriptor in (read_end, pipe):
            self.addCleanup(os.close, descriptor)
        fill(pipe)
        bench, folder = self.start("none", "--orders=2", "--window=2", "--csv=/dev/stdout", stdout=pipe, stderr=pipe)
        time.sleep(0.5)  # enough for a client that wrote the CSV only later to have started the exchange
        os.kill(bench.pid, signal.SIGTERM)
        self.assertEqual(self.finish(bench, timeout=10)[0], -signal.SIGTERM)
        self.assertFalse((folder / "exchange.pid").exists())  # the CSV header blocked before the spawn

    def test_fails_when_the_csv_is_the_config(self):
        # The config replaced by the CSV header, so the exchange cannot start, instead of config text in the CSV.
        bench, folder = self.start(None, "--orders=2", "--window=2", "--csv=wire_latency.conf")
        code, errors = self.finish(bench)
        self.assertEqual(code, 1, errors)
        self.assertIn("exsim_wire_bench: the exchange exited before its ready line\n", errors)
        self.assertTrue((folder / "wire_latency.conf").read_text().startswith("Benchmark,"))

    def test_fails_when_stdout_or_stderr_is_closed_at_launch(self):
        # A file opened later would take the closed number, and the report or an error would go into it.
        message = "exsim_wire_bench: stdout and stderr must be open\n"
        for name, descriptor, errors in [("stdout", 1, message), ("stderr", 2, "")]:
            with self.subTest(name):
                bench, folder = self.start("none", "--orders=2", "--window=2",
                                           preexec_fn=lambda descriptor=descriptor: os.close(descriptor))
                self.assertEqual(self.finish(bench), (1, errors))
                self.assertFalse((folder / "wire_results.csv").exists())

    @unittest.skipUnless(Path("/dev/full").exists(), "needs /dev/full")
    def test_fails_when_the_csv_cannot_be_written(self):
        bench, _ = self.start("none", "--orders=2", "--csv=/dev/full")
        self.assertEqual(self.finish(bench)[0], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
