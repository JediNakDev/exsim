"""FIX 4.4 interop check: a QuickFIX initiator against exsim_exchanged.

QuickFIX validates everything the exchange sends against its own FIX44.xml
(required fields, values, field order, checksum, body length, sequence
numbers). Any Reject it sends back, or any message it cannot parse, fails the
check. Needs the `quickfix` Python package; see test/README.md.

    python test/interop/fix44_quickfix.py [LOG_DIR]
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

import quickfix as fix

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / os.environ.get("EXSIM_BUILD", "build/release")
CONFIG = Path(__file__).with_suffix(".conf")
DICTIONARY = Path(sys.prefix) / "share/quickfix/FIX44.xml"  # installed by pip with the package
PASSWORDS = {"ALICE": "wrong", "BOB": "bob"}  # scenario() corrects ALICE's after her refused Logon


def fields(message):
    return dict(field.split("=", 1) for field in message.toString().split("\x01") if field)


class Recorder(fix.Application):
    """Every message in or out, per user, in arrival order."""

    def __init__(self):
        super().__init__()
        self.changed = threading.Condition()
        self.events = []  # (user, "in" | "out" | "logon" | "logout", fields)
        self.sessions = {}

    def record(self, session, kind, message=None):
        with self.changed:
            self.events.append((session.getSenderCompID().getValue(), kind, fields(message) if message else {}))
            self.changed.notify_all()

    def onCreate(self, session):
        self.sessions[session.getSenderCompID().getValue()] = session

    def onLogon(self, session):
        self.record(session, "logon")

    def onLogout(self, session):
        self.record(session, "logout")

    def toAdmin(self, message, session):
        if message.getHeader().getField(35) == "A":
            message.setField(554, PASSWORDS[session.getSenderCompID().getValue()])
        self.record(session, "out", message)

    def fromAdmin(self, message, session):
        self.record(session, "in", message)

    def toApp(self, message, session):
        self.record(session, "out", message)

    def fromApp(self, message, session):
        self.record(session, "in", message)

    def mark(self):
        with self.changed:
            return len(self.events)

    def wait(self, user, kind, after, what, **expected):
        """First event of `user` and `kind` after `after` whose fields include `expected`."""
        def match():
            return next((values for event_user, event_kind, values in self.events[after:]
                         if event_user == user and event_kind == kind
                         and all(values.get(tag[1:]) == value for tag, value in expected.items())), None)
        with self.changed:
            if not self.changed.wait_for(lambda: match() is not None, timeout=5):
                raise AssertionError(f"{user}: no {what}")
            return match()

    def send(self, user, message_type, *body):
        message = fix.Message()
        message.getHeader().setField(35, message_type)
        for tag, value in body:
            message.setField(tag, str(value))
        if message_type in "DFGq":
            message.setField(fix.TransactTime())
        if not fix.Session.sendToTarget(message, self.sessions[user]):
            raise AssertionError(f"{user}: QuickFIX did not send {message_type}")


def order(token, side, quantity, price):
    return (11, token), (55, "AAPL"), (54, side), (38, quantity), (40, "2"), (44, price), (59, "0")


def scenario(app, exchange):
    # A refused Logon gets no reply, so the next one still starts at MsgSeqNum 1.
    app.wait("ALICE", "logout", 0, "refused Logon")
    if [values for user, kind, values in app.events if user == "ALICE" and kind == "in"]:
        raise AssertionError("ALICE: refused Logon got a reply")
    PASSWORDS["ALICE"] = "alice"
    for user in PASSWORDS:
        app.wait(user, "logon", 0, "logon")
    print("ok: refused Logon, then both sessions logged on")

    mark = app.mark()
    app.send("ALICE", "D", *order("A1", "1", 100, "150"))
    app.wait("ALICE", "in", mark, "ExecutionReport New", _35="8", _11="A1", _150="0", _39="0", _151="100")
    mark = app.mark()
    app.send("ALICE", "G", (41, "A1"), *order("A2", "1", 150, "150.25"))
    app.wait("ALICE", "in", mark, "ExecutionReport Replaced", _35="8", _11="A2", _41="A1", _150="5", _38="150")
    print("ok: NewOrderSingle and OrderCancelReplaceRequest")

    mark = app.mark()
    app.send("BOB", "D", *order("B1", "2", 50, "150"))
    app.wait("BOB", "in", mark, "fill", _35="8", _11="B1", _150="F", _39="2", _32="50", _31="150.25")
    app.wait("ALICE", "in", mark, "fill", _35="8", _11="A2", _150="F", _39="1", _14="50", _151="100", _6="150.25")
    print("ok: fills on both sessions")

    mark = app.mark()
    app.send("ALICE", "F", (41, "A2"), (11, "A3"), (55, "AAPL"), (54, "1"), (38, 150))
    app.wait("ALICE", "in", mark, "ExecutionReport Canceled", _35="8", _11="A3", _41="A2", _150="4", _39="4")
    mark = app.mark()
    app.send("ALICE", "F", (41, "NOPE"), (11, "A4"), (55, "AAPL"), (54, "1"), (38, 1))
    app.wait("ALICE", "in", mark, "OrderCancelReject", _35="9", _11="A4", _434="1", _102="1")
    print("ok: OrderCancelRequest and OrderCancelReject")

    mark = app.mark()
    app.send("ALICE", "q", (11, "A5"), (530, "7"))
    app.wait("ALICE", "in", mark, "OrderMassCancelReport", _35="r", _11="A5", _531="7", _533="0")
    app.send("ALICE", "D", (11, "A6"), (55, "AAPL"), (38, 1), (40, "2"), (44, "150"))
    app.wait("ALICE", "in", mark, "Reject for missing Side", _35="3", _371="54", _373="1")
    app.send("ALICE", "H", (11, "A7"), (55, "AAPL"), (54, "1"))
    app.wait("ALICE", "in", mark, "BusinessMessageReject", _35="j", _372="H", _380="3")
    print("ok: OrderMassCancelReport, Reject and BusinessMessageReject")

    mark = app.mark()
    app.send("ALICE", "1", (112, "PING"))
    app.wait("ALICE", "in", mark, "Heartbeat for TestRequest", _35="0", _112="PING")
    mark = app.mark()
    time.sleep(2.5)
    app.wait("ALICE", "in", mark, "idle Heartbeat", _35="0")
    print("ok: TestRequest and Heartbeat")

    # ALICE leaves an order resting and logs out; BOB fills it while she is
    # away. She also forgets the exchange's Logout reply, as a client that
    # did not persist it would. Her next Logon reply is ahead of her expected
    # MsgSeqNum, so QuickFIX asks for a resend: the Logout comes back as a
    # GapFill and the fill as a PossDup report.
    alice = fix.Session.lookupSession(app.sessions["ALICE"])
    mark = app.mark()
    app.send("ALICE", "D", *order("A8", "1", 10, "150"))
    app.wait("ALICE", "in", mark, "ExecutionReport New", _35="8", _11="A8", _150="0")
    alice.logout()
    app.wait("ALICE", "in", mark, "Logout reply", _35="5")
    app.wait("ALICE", "logout", mark, "logout")
    app.send("BOB", "D", *order("B2", "2", 10, "150"))
    app.wait("BOB", "in", mark, "fill", _35="8", _11="B2", _150="F", _39="2")
    logout_reply = alice.getExpectedTargetNum() - 1
    alice.setNextTargetMsgSeqNum(logout_reply)
    alice.logon()
    app.wait("ALICE", "out", mark, "ResendRequest", _35="2", _7=str(logout_reply), _16="0")
    app.wait("ALICE", "in", mark, "GapFill", _35="4", _34=str(logout_reply), _43="Y", _123="Y")
    app.wait("ALICE", "in", mark, "resent fill", _35="8", _11="A8", _150="F", _43="Y")
    app.wait("ALICE", "logon", mark, "logon after resend")
    print("ok: exchange answers ResendRequest with GapFill and PossDup report")

    # ALICE skips a MsgSeqNum: the exchange holds her order, asks for a
    # resend, and takes QuickFIX's GapFill; the PossDup copy is ignored.
    mark = app.mark()
    alice.setNextSenderMsgSeqNum(alice.getExpectedSenderNum() + 1)
    app.send("ALICE", "D", *order("A9", "1", 10, "149"))
    request = app.wait("ALICE", "in", mark, "ResendRequest", _35="2")
    app.wait("ALICE", "out", mark, "GapFill", _35="4", _34=request["7"], _123="Y")
    app.wait("ALICE", "out", mark, "PossDup order", _35="D", _11="A9", _43="Y")
    app.wait("ALICE", "in", mark, "ExecutionReport New", _35="8", _11="A9", _150="0")
    app.send("ALICE", "1", (112, "AFTER"))
    app.wait("ALICE", "in", mark, "Heartbeat", _35="0", _112="AFTER")
    if [values for user, kind, values in app.events[mark:] if kind == "in" and values.get("11") == "A9"
            and values.get("150") != "0"]:
        raise AssertionError("ALICE: order A9 was processed twice")
    print("ok: QuickFIX answers the exchange's ResendRequest with GapFill")

    mark = app.mark()
    alice.logout()
    app.wait("ALICE", "in", mark, "Logout reply", _35="5")
    app.wait("ALICE", "logout", mark, "logout")
    exchange.terminate()
    app.wait("BOB", "in", mark, "exchange Logout", _35="5")
    app.wait("BOB", "out", mark, "Logout reply", _35="5")
    app.wait("BOB", "logout", mark, "logout")
    if exchange.wait(timeout=10) != 0:
        raise AssertionError(f"exchange exited with {exchange.returncode}")
    print("ok: Logout from either side")


def main():
    # A new directory each run: QuickFIX keeps its sequence numbers there.
    log_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix="exsim-quickfix-"))
    log_dir.mkdir(parents=True, exist_ok=len(sys.argv) == 1)
    exchange = subprocess.Popen([str(BUILD / "exsim_exchanged"), "--config", str(CONFIG)],
                                stdout=subprocess.PIPE, text=True)
    initiator = None
    try:
        port = json.loads(exchange.stdout.readline())["fixOrderEntryPort"]
        settings_file = log_dir / "quickfix.cfg"
        settings_file.write_text(f"""[DEFAULT]
ConnectionType=initiator
BeginString=FIX.4.4
TargetCompID=EXSIM
SocketConnectHost=127.0.0.1
SocketConnectPort={port}
HeartBtInt=1
ReconnectInterval=1
StartTime=00:00:00
EndTime=00:00:00
UseDataDictionary=Y
DataDictionary={DICTIONARY}
ValidateLengthAndChecksum=Y
ValidateFieldsOutOfOrder=Y
ValidateFieldsHaveValues=Y
ValidateUserDefinedFields=Y
AllowUnknownMsgFields=N
CheckCompID=Y
CheckLatency=Y
FileLogPath={log_dir}
FileStorePath={log_dir}
""" + "".join(f"[SESSION]\nSenderCompID={user}\n" for user in PASSWORDS))
        app = Recorder()
        settings = fix.SessionSettings(str(settings_file))
        initiator = fix.SocketInitiator(app, fix.FileStoreFactory(settings), settings, fix.FileLogFactory(settings))
        initiator.start()
        scenario(app, exchange)
        problems = [f"{user} sent {values}" for user, kind, values in app.events
                    if kind == "out" and values.get("35") in ("3", "j")]
        problems += [f"{path.name}: {line}" for path in log_dir.glob("*.event.current.log")
                     for line in path.read_text().splitlines()
                     if "Invalid" in line or "rejected" in line.lower()]
        if problems:
            raise AssertionError("QuickFIX found problems:\n" + "\n".join(problems))
        print(f"PASS (QuickFIX logs in {log_dir})")
        return 0
    except AssertionError as error:
        print(f"FAIL: {error}\nQuickFIX logs in {log_dir}")
        return 1
    finally:
        if initiator is not None:
            initiator.stop()
        exchange.terminate()
        exchange.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
