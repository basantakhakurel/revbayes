#!/usr/bin/env python3
"""Conformance tests for `rb --server` (protocol 1).

Standard library only.

    python3 tests/server/run_conformance.py PATH_TO_RB [-v] [-k SUBSTRING]

Set RB_WRAPPER to run the binary through a launcher, for example RB_WRAPPER=wine for a Windows build.

These tests are the executable form of the protocol described in section 6.4 of GUI_Implementation_Note.md.
Phase 0 covers the framing, the handshake and the commands the phase 0 stub implements. The tests that need the
interpreter behind the protocol (submit, snapshots, output capture, quit, ask, interrupt) arrive with phase 1;
see README.md in this directory.
"""

import argparse
import json
import os
import queue
import shlex
import subprocess
import sys
import tempfile
import threading
import unittest

RB = None            # path of the rb executable, set from the command line
WRAPPER = []         # optional launcher, from RB_WRAPPER
TIMEOUT = 60         # seconds: interpreter start-up is fast, CI machines and wine are not


def command(*args):
    return WRAPPER + [RB] + list(args)


class Server:
    """A running `rb --server` with its stdout parsed into protocol events."""

    def __init__(self, args=("--server",), cwd=None):
        self.proc = subprocess.Popen(command(*args), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, cwd=cwd)
        self._lines = queue.Queue()
        self.stderr = []
        threading.Thread(target=self._pump_stdout, daemon=True).start()
        threading.Thread(target=self._pump_stderr, daemon=True).start()

    def _pump_stdout(self):
        for line in iter(self.proc.stdout.readline, b""):
            self._lines.put(line)
        self._lines.put(None)

    def _pump_stderr(self):
        for line in iter(self.proc.stderr.readline, b""):
            self.stderr.append(line.decode("utf-8", "replace"))

    def send(self, obj):
        self.send_raw(json.dumps(obj).encode("utf-8") + b"\n")

    def send_raw(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def recv(self, timeout=TIMEOUT):
        """Next event. Also checks the wire format: valid UTF-8, one JSON object per line, LF line endings."""
        try:
            line = self._lines.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError("timed out after %ss waiting for a message" % timeout)
        if line is None:
            raise AssertionError("the server closed stdout (exit code %r, stderr: %r)"
                                 % (self.proc.poll(), "".join(self.stderr)[-300:]))
        assert line.endswith(b"\n"), "message not terminated by a newline: %r" % line
        assert not line.endswith(b"\r\n"), "message ends with CRLF, stdout must be in binary mode: %r" % line
        event = json.loads(line.decode("utf-8"))
        assert isinstance(event, dict), "message is not a JSON object: %r" % line
        return event

    def expect_silence(self, seconds):
        try:
            line = self._lines.get(timeout=seconds)
        except queue.Empty:
            return
        raise AssertionError("unexpected output: %r" % (line,))

    def wait_exit(self, timeout=TIMEOUT):
        return self.proc.wait(timeout)

    def close(self):
        try:
            if self.proc.stdin and not self.proc.stdin.closed:
                self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        for stream in (self.proc.stdout, self.proc.stderr):
            if stream:
                stream.close()


class ServerCase(unittest.TestCase):

    def start(self, **kwargs):
        server = Server(**kwargs)
        self.addCleanup(server.close)
        return server

    def hello(self, server, protocol=1, request_id=1):
        server.send({"id": request_id, "cmd": "hello", "protocol": protocol, "client": "conformance"})
        return server.recv()

    def ready(self, **kwargs):
        server = self.start(**kwargs)
        reply = self.hello(server)
        self.assertEqual(reply["ev"], "hello")
        return server

    # --- command line -----------------------------------------------------------------------------------------

    def test_server_info(self):
        result = subprocess.run(command("--server-info"), capture_output=True, timeout=TIMEOUT)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.decode("utf-8").splitlines()
        self.assertEqual(len(lines), 1, "--server-info must print exactly one line")
        info = json.loads(lines[0])
        self.assertEqual(info["protocol"], 1)
        self.assertTrue(info["version"].startswith("RevBayes "), info["version"])
        self.assertIsInstance(info["features"], list)

    def test_incompatible_flags_are_refused(self):
        for extra in (["-e", "1"], ["--jupyter"], ["--interactive"]):
            result = subprocess.run(command("--server", *extra), input=b"", capture_output=True, timeout=TIMEOUT)
            self.assertNotEqual(result.returncode, 0, extra)
            self.assertIn(b"--server", result.stderr, extra)
            self.assertEqual(result.stdout, b"", "nothing may be written to stdout on a refusal: %r" % extra)

    # --- handshake --------------------------------------------------------------------------------------------

    def test_server_does_not_speak_first(self):
        server = self.start()
        server.expect_silence(0.5)

    def test_hello_handshake(self):
        with tempfile.TemporaryDirectory() as workdir:
            server = self.start(cwd=workdir)
            reply = self.hello(server, request_id=7)
            self.assertEqual(reply["ev"], "hello")
            self.assertEqual(reply["re"], 7)
            self.assertEqual(reply["protocol"], 1)
            self.assertTrue(reply["server"].startswith("RevBayes "), reply["server"])
            self.assertIsInstance(reply["pid"], int)
            self.assertIsInstance(reply["features"], list)
            self.assertTrue(os.path.samefile(reply["cwd"], workdir), (reply["cwd"], workdir))

    def test_hello_reports_non_ascii_cwd(self):
        prefix = "rb-éü-ディ-"
        try:
            workdir = tempfile.mkdtemp(prefix=prefix)
        except (OSError, UnicodeError):
            self.skipTest("cannot create a non-ASCII directory on this file system")
        self.addCleanup(os.rmdir, workdir)
        server = self.start(cwd=workdir)
        reply = self.hello(server)
        self.assertTrue(os.path.samefile(reply["cwd"], workdir), (reply["cwd"], workdir))

    def test_hello_protocol_must_be_an_integer(self):
        server = self.start()
        server.send({"id": 1, "cmd": "hello", "protocol": "1"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"], reply["re"]), ("error", "bad_request", 1))

    def test_protocol_from_the_future_is_refused_and_the_server_exits(self):
        server = self.start()
        reply = self.hello(server, protocol=99)
        self.assertEqual((reply["ev"], reply["code"], reply["re"]), ("error", "protocol_unsupported", 1))
        self.assertEqual(server.wait_exit(), 2)

    def test_hello_is_required_first(self):
        server = self.start()
        server.send({"id": 1, "cmd": "ping"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"], reply["re"]), ("error", "bad_request", 1))
        self.assertEqual(self.hello(server, request_id=2)["ev"], "hello")     # the server carries on

    def test_a_second_hello_is_rejected(self):
        server = self.ready()
        reply = self.hello(server, request_id=2)
        self.assertEqual((reply["ev"], reply["code"], reply["re"]), ("error", "bad_request", 2))

    # --- commands available in every state --------------------------------------------------------------------

    def test_ping(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "ping"})
        self.assertEqual(server.recv(), {"ev": "pong", "re": 5})

    def test_interrupt_while_idle(self):
        server = self.ready()
        server.send({"id": 6, "cmd": "interrupt"})
        self.assertEqual(server.recv(), {"ev": "ack", "of": "interrupt", "was_busy": False, "re": 6})

    def test_shutdown(self):
        server = self.ready()
        server.send({"id": 9, "cmd": "shutdown"})
        self.assertEqual(server.recv(), {"ev": "bye", "reason": "shutdown", "re": 9})
        self.assertEqual(server.wait_exit(), 0)

    def test_end_of_input_ends_the_server(self):
        server = self.ready()
        server.proc.stdin.close()
        self.assertEqual(server.wait_exit(), 0)

    # --- malformed input --------------------------------------------------------------------------------------

    def test_unknown_command(self):
        server = self.ready()
        server.send({"id": 3, "cmd": "frobnicate"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"], reply["re"]), ("error", "unknown_command", 3))

    def test_malformed_requests_get_errors_and_the_server_survives(self):
        server = self.ready()

        server.send_raw(b"this is not json\n")
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"]), ("error", "bad_json"))
        self.assertNotIn("re", reply)

        server.send_raw(b"[1, 2]\n")
        self.assertEqual(server.recv()["code"], "bad_request")

        server.send({"id": 5})                                    # no cmd
        reply = server.recv()
        self.assertEqual((reply["code"], reply["re"]), ("bad_request", 5))

        server.send({"id": 6, "cmd": 7})                          # cmd is not a string
        reply = server.recv()
        self.assertEqual((reply["code"], reply["re"]), ("bad_request", 6))

        server.send({"id": 8, "cmd": "ping"})                     # still alive
        self.assertEqual(server.recv(), {"ev": "pong", "re": 8})

    def test_blank_lines_are_ignored_and_crlf_is_accepted(self):
        server = self.ready()
        server.send_raw(b"\n\n" + json.dumps({"id": 4, "cmd": "ping"}).encode() + b"\r\n")
        self.assertEqual(server.recv(), {"ev": "pong", "re": 4})

    def test_oversized_request_is_rejected_and_the_server_survives(self):
        server = self.ready()
        server.send_raw(b'{"id":1,"cmd":"ping","pad":"' + b"x" * (17 * 1024 * 1024) + b'"}\n')
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"]), ("error", "bad_request"))
        server.send({"id": 2, "cmd": "ping"})
        self.assertEqual(server.recv(), {"ev": "pong", "re": 2})

    # --- protocol 1 commands that the phase 0 stub does not implement -----------------------------------------

    def test_commands_of_later_phases_are_answered_not_ignored(self):
        """Each must get a reply carrying the request id: `not_implemented` from the stub, a real reply later."""
        server = self.ready()
        for number, cmd in enumerate(["submit", "snapshot", "inspect", "complete", "help", "set", "answer"], start=10):
            server.send({"id": number, "cmd": cmd})
            reply = server.recv()
            self.assertEqual(reply.get("re"), number, (cmd, reply))
            if reply["ev"] == "error":
                self.assertIn(reply["code"], ("not_implemented", "bad_request", "busy"), (cmd, reply))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rb", help="path of the rb executable")
    parser.add_argument("-v", "--verbose", action="store_true", help="print each test name")
    parser.add_argument("-k", dest="patterns", action="append", metavar="SUBSTRING",
                        help="only run tests whose name contains SUBSTRING (repeatable)")
    args = parser.parse_args(argv)

    global RB, WRAPPER
    RB = os.path.abspath(args.rb)
    WRAPPER = shlex.split(os.environ.get("RB_WRAPPER", ""))
    if not os.path.exists(RB):
        parser.error("no such file: %s" % RB)

    loader = unittest.TestLoader()
    if args.patterns:
        loader.testNamePatterns = ["*%s*" % p for p in args.patterns]
    suite = loader.loadTestsFromTestCase(ServerCase)
    result = unittest.TextTestRunner(verbosity=2 if args.verbose else 1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
