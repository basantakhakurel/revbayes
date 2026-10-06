#!/usr/bin/env python3
"""Conformance tests for `rb --server` (protocol 1).

Standard library only.

    python3 tests/server/run_conformance.py PATH_TO_RB [-v] [-k SUBSTRING]

Set RB_WRAPPER to run the binary through a launcher, for example RB_WRAPPER=wine for a Windows build.

These tests are the executable form of the protocol described in section 6.4 of GUI_Implementation_Note.md.
Framing, the handshake, `submit` (output streaming, continuation, errors, busy rejection, quit()), `snapshot`
(`variables`/`functions`/`all`, via WorkspaceSnapshot), the `ask`/`answer` exchange (a function redefinition
mid-submit, routed through the protocol instead of reading stdin directly -- see RevServer::requestAsk),
`inspect` (structure() capture, bounded to 64 KiB), `complete` (Completion.h, shared with the terminal client)
and `help` (RbHelpSystem/RbHelpDatabase) are covered. Only `set` is still not implemented. Real interruption of a
running submit also does not exist yet (`interrupt` only reports whether the interpreter was busy); C4 in
GUI_Implementation_Note.md is needed for that.
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

    def submit(self, server, text, request_id, timeout=TIMEOUT):
        """Sends `submit`, collects every `output` event for it, and returns (done_event, joined_output_text,
        variables_rows). Stops at `done`; also consumes the `variables` event that the protocol says follows it
        (section 6.4) and returns its `rows` (None if `done` was itself an `error`, since no `variables` follows
        that -- see processSubmit/handleLine in RevServer.cpp)."""
        server.send({"id": request_id, "cmd": "submit", "text": text})
        return self.finishSubmit(server, request_id, timeout)

    def finishSubmit(self, server, request_id, timeout=TIMEOUT):
        """The tail half of submit(), split out for a caller that already consumed a leading event of its own
        (for example an `ask`) by hand and now wants the rest of that submit's events collected the same way."""
        chunks = []
        while True:
            event = server.recv(timeout)
            if event.get("ev") == "output" and event.get("id") == request_id:
                chunks.append(event["text"])
            elif event.get("ev") == "done" and event.get("re") == request_id:
                variables = server.recv(timeout)
                self.assertEqual(variables.get("ev"), "variables", variables)
                return event, "".join(chunks), variables["rows"]
            elif event.get("ev") == "error" and event.get("re") == request_id:
                return event, "".join(chunks), None

    def triggerAsk(self, server, base_id):
        """Defines a function f, then submits a redefinition of f with the same signature -- which calls
        UserInterface::ask() (SyntaxFunctionDef.cpp:176-178) and so, mid-submit, produces an `ask` event instead
        of the eventual `done` (defect D6/D7, GUI_Implementation_Note.md section 6.3). Returns (ask_event,
        redefine_request_id); the caller still owes that submit an `answer` (or must abandon it) before
        finishSubmit() will return."""
        self.submit(server, "function f(x) { return x }", base_id)
        redefine_id = base_id + 1
        server.send({"id": redefine_id, "cmd": "submit", "text": "function f(x) { return x + 1 }"})
        ask = server.recv()
        self.assertEqual(ask["ev"], "ask", ask)
        return ask, redefine_id

    def snapshot(self, server, request_id, what="variables", timeout=TIMEOUT):
        """Sends `snapshot` and returns the reply event (a `variables` event, or an `error`)."""
        server.send({"id": request_id, "cmd": "snapshot", "what": what})
        return server.recv(timeout)

    def rowsByName(self, rows):
        """Rows keyed by name, for convenient lookup in assertions; fails loudly on an unexpected duplicate name
        rather than silently keeping only one (which a plain dict comprehension would do)."""
        by_name = {}
        for row in rows:
            self.assertNotIn(row["name"], by_name, ("duplicate row name", row, rows))
            by_name[row["name"]] = row
        return by_name

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

    # --- submit ------------------------------------------------------------------------------------------------

    def test_submit_runs_rev_code_and_streams_output(self):
        server = self.ready()
        done, output, _ = self.submit(server, "1+1", 5)
        self.assertEqual(done["status"], "ok")
        self.assertEqual(done["rc"], 0)
        self.assertIn("cwd", done)
        self.assertIsInstance(done["elapsed_ms"], int)
        self.assertIn("2", output)

    def test_submit_continuation_across_several_requests(self):
        """An incomplete statement (rc 1) is completed by the NEXT submit; the server owns the accumulated text."""
        server = self.ready()
        done, _, _rows = self.submit(server, "for (i in 1:2) {", 5)
        self.assertEqual(done["status"], "incomplete")
        self.assertEqual(done["rc"], 1)
        done, _, _rows = self.submit(server, "print(i)", 6)
        self.assertEqual(done["status"], "incomplete")
        done, output, _ = self.submit(server, "}", 7)
        self.assertEqual(done["status"], "ok")
        self.assertEqual(output, "1\n2\n")

    def test_submit_reports_a_syntax_error_without_killing_the_server(self):
        server = self.ready()
        done, output, rows = self.submit(server, "this is not )))" , 5)
        self.assertEqual(done["status"], "error")
        self.assertEqual(done["rc"], 2)
        self.assertIn("Syntax error", output)
        # the server is still usable afterwards
        done, output, _ = self.submit(server, "1+1", 6)
        self.assertEqual(done["status"], "ok")

    def test_submit_round_trips_non_ascii_output(self):
        server = self.ready()
        text = "éü" + "ディ" + "\U0001F600"   # accented Latin, katakana, an emoji (4-byte UTF-8)
        done, output, _ = self.submit(server, 's <- "' + text + '"; print(s)', 5)
        self.assertEqual(done["status"], "ok")
        self.assertIn(text, output)

    def test_setwd_to_a_non_ascii_path_is_reported_correctly(self):
        """4.7: the working directory is process-global and reported via current_path() after every submit, not
        just at hello time (test_hello_reports_non_ascii_cwd covers that one)."""
        server = self.ready()
        prefix = "rb-éü-ディ-"
        try:
            workdir = tempfile.mkdtemp(prefix=prefix)
        except (OSError, UnicodeError):
            self.skipTest("cannot create a non-ASCII directory on this file system")
        self.addCleanup(os.rmdir, workdir)

        escaped = workdir.replace("\\", "\\\\").replace('"', '\\"')
        done, _, _ = self.submit(server, 'setwd("' + escaped + '")', 5)
        self.assertEqual(done["status"], "ok")
        self.assertTrue(os.path.samefile(done["cwd"], workdir), (done["cwd"], workdir))

    def test_submit_needs_a_text_field(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "submit"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"], reply["re"]), ("error", "bad_request", 5))

    def test_a_second_submit_is_rejected_as_busy(self):
        """A well-behaved client waits for `done`; a client that does not gets `busy`, not silent queuing."""
        server = self.ready()
        server.send({"id": 5, "cmd": "submit", "text": "1+1"})
        server.send({"id": 6, "cmd": "submit", "text": "2+2"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"], reply["re"]), ("error", "busy", 6))
        # the first submit still completes normally afterwards
        while True:
            event = server.recv()
            if event.get("ev") == "done" and event.get("re") == 5:
                self.assertEqual(event["status"], "ok")
                break

    def test_ping_and_interrupt_are_answered_while_busy(self):
        """The reader thread never blocks on the interpreter: fast-path commands are answered immediately."""
        server = self.ready()
        server.send({"id": 5, "cmd": "submit", "text": "1+1"})
        server.send({"id": 6, "cmd": "ping"})
        server.send({"id": 7, "cmd": "interrupt"})
        seen = {}
        while len(seen) < 3:
            event = server.recv()
            key = event.get("re")
            if key in (5, 6, 7):
                seen[key] = event
        self.assertEqual(seen[6], {"ev": "pong", "re": 6})
        self.assertEqual(seen[7]["ev"], "ack")
        self.assertTrue(seen[7]["was_busy"])   # the submit was still running when interrupt was answered
        self.assertEqual(seen[5]["status"], "ok")

    def test_quit_reports_itself_then_the_process_exits_cleanly(self):
        """Covers the exact scenario that deadlocked during development: quit() while the reader thread is still
        blocked reading stdin (i.e. the client has not closed its end). See RevClient.cpp's quitRequestHandler."""
        server = self.ready()
        server.send({"id": 5, "cmd": "submit", "text": "quit()"})
        seen = []
        while True:
            event = server.recv()
            seen.append(event.get("ev"))
            if event.get("ev") == "bye":
                break
        self.assertEqual(seen, ["quit", "bye"])
        self.assertEqual(server.wait_exit(), 0)

    # --- ask/answer (S5) ----------------------------------------------------------------------------------------

    def test_ask_and_answer_redefines_the_function(self):
        """The D.4 example in GUI_Implementation_Note.md, answered with `value: true`: the redefinition takes
        effect."""
        server = self.ready()
        ask, redefine_id = self.triggerAsk(server, 10)
        self.assertEqual(ask["question"], "Replace existing function with same signature")
        self.assertNotIn("re", ask)   # a server-initiated event, not a reply to redefine_id -- own id space

        server.send({"id": redefine_id + 1, "cmd": "answer", "re": ask["id"], "value": True})
        done, _, _ = self.finishSubmit(server, redefine_id)
        self.assertEqual(done["status"], "ok")

        done, output, _ = self.submit(server, "print(f(1))", redefine_id + 2)
        self.assertEqual(done["status"], "ok")
        self.assertIn("2", output)   # x + 1: the redefinition took effect

    def test_answer_false_declines_the_redefinition(self):
        server = self.ready()
        ask, redefine_id = self.triggerAsk(server, 10)

        server.send({"id": redefine_id + 1, "cmd": "answer", "re": ask["id"], "value": False})
        done, output, _ = self.finishSubmit(server, redefine_id)
        self.assertEqual(done["status"], "ok")   # declining is not a parse error, just a skipped registration
        self.assertIn("canceled", output)

        done, output, _ = self.submit(server, "print(f(1))", redefine_id + 2)
        self.assertEqual(done["status"], "ok")
        self.assertIn("1", output)   # the original definition (return x) is still in effect

    def test_answer_re_must_match_the_pending_ask(self):
        server = self.ready()
        ask, redefine_id = self.triggerAsk(server, 10)

        server.send({"id": redefine_id + 1, "cmd": "answer", "re": ask["id"] + 1000, "value": True})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"]), ("error", "bad_request"))

        # the mismatched answer did not consume the pending ask; a correctly-addressed one still completes it
        server.send({"id": redefine_id + 2, "cmd": "answer", "re": ask["id"], "value": True})
        done, _, _ = self.finishSubmit(server, redefine_id)
        self.assertEqual(done["status"], "ok")

    def test_answer_needs_a_boolean_value_field(self):
        server = self.ready()
        ask, redefine_id = self.triggerAsk(server, 10)

        server.send({"id": redefine_id + 1, "cmd": "answer", "re": ask["id"]})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"]), ("error", "bad_request"))

        server.send({"id": redefine_id + 2, "cmd": "answer", "re": ask["id"], "value": True})
        done, _, _ = self.finishSubmit(server, redefine_id)
        self.assertEqual(done["status"], "ok")

    def test_answer_without_a_pending_ask_is_rejected(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "answer", "re": 1, "value": True})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"], reply["message"]),
                         ("error", "bad_request", "no ask is pending"))

    def test_ask_is_abandoned_without_hanging_if_the_client_disappears(self):
        """The interpreter thread must never block forever in requestAsk(): if the front end vanishes (here,
        stdin EOF) while a redefinition is waiting for confirmation, the ask is abandoned (treated as declined)
        and the submit -- and then the process -- still complete normally, well under the 10s watchdog
        (RevServer::requestStop(), which wakes a pending requestAsk() the same way it wakes the interpreter
        loop in run())."""
        server = self.ready()
        _, redefine_id = self.triggerAsk(server, 10)

        server.proc.stdin.close()
        done, output, _ = self.finishSubmit(server, redefine_id)
        self.assertEqual(done["status"], "ok")
        self.assertIn("canceled", output)
        self.assertEqual(server.wait_exit(), 0)

    # --- snapshot (S6) -----------------------------------------------------------------------------------------

    def test_variables_after_submit_reports_kinds_and_the_system_args_variable(self):
        server = self.ready()
        _, _, rows = self.submit(server, "a <- 1.5; y ~ dnExp(1); z := y * 2", 5)
        by_name = self.rowsByName(rows)

        self.assertEqual(by_name["a"]["kind"], "constant")
        self.assertEqual(by_name["a"]["type"], "RealPos")
        self.assertEqual(by_name["a"]["summary"], "1.5")
        self.assertEqual(by_name["a"]["flags"], [])

        self.assertEqual(by_name["y"]["kind"], "stochastic")
        self.assertEqual(by_name["y"]["children"], 1)   # z depends on y

        self.assertEqual(by_name["z"]["kind"], "deterministic")

        # created on every start-up by RevLanguageMain, not by anything this test asked for (section 4.12)
        self.assertEqual(by_name["args"]["flags"], ["system"])
        self.assertEqual(by_name["args"]["kind"], "constant")

    def test_variables_reports_a_clamped_node(self):
        server = self.ready()
        _, _, rows = self.submit(server, "y ~ dnExp(1); y.clamp(0.5)", 5)
        self.assertEqual(self.rowsByName(rows)["y"]["kind"], "clamped")

    def test_variables_follows_a_reference_variable_instead_of_showing_null(self):
        """The bug this whole feature was built to fix: `ls()` shows a reference variable's value as the literal
        string NULL (GUI_Implementation_Note.md, section 4.6, verified against the real ls() during the initial
        assessment). The protocol's own snapshot must not repeat that."""
        server = self.ready()
        _, _, rows = self.submit(server, "a <- 1.5; b <-& a", 5)
        by_name = self.rowsByName(rows)
        self.assertEqual(by_name["b"]["flags"], ["reference"])
        self.assertEqual(by_name["b"]["type"], "RealPos")
        self.assertEqual(by_name["b"]["summary"], "1.5")   # NOT "NULL"

    def test_variables_nests_element_variables_under_their_parent(self):
        server = self.ready()
        _, _, rows = self.submit(server, "x[1] <- 1; x[2] <- 2", 5)
        by_name = self.rowsByName(rows)
        self.assertEqual(by_name["x[1]"]["parent"], "x")
        self.assertEqual(by_name["x[1]"]["flags"], ["element"])
        self.assertEqual(by_name["x[1]"]["summary"], "1")
        self.assertEqual(by_name["x[2]"]["parent"], "x")
        # x itself: a not-yet-built vector variable is valid Rev, not a crash -- see the next test for the case
        # where building it actually fails.
        self.assertIn("x", by_name)

    def test_variables_degrades_gracefully_for_an_unbuildable_vector_variable(self):
        """x[3] is never set, so building the vector "x" from its elements fails; the snapshot must report that
        one row as unusable rather than losing the whole snapshot (or crashing) over it."""
        server = self.ready()
        _, _, rows = self.submit(server, "x[1] <- 1; x[2] <- 2; x[4] <- 4", 5)
        by_name = self.rowsByName(rows)
        self.assertEqual(by_name["x"]["kind"], "unknown")
        self.assertIn("x[3]", by_name["x"]["summary"])
        # the elements that DO exist are still reported normally
        self.assertEqual(by_name["x[1]"]["summary"], "1")
        self.assertEqual(by_name["x[4]"]["summary"], "4")

    def test_variables_summary_is_bounded_in_space_and_time_for_a_huge_container(self):
        server = self.ready()
        _, _, rows = self.submit(server, "huge <- rep(1, 2000000)", 5, timeout=20)
        row = self.rowsByName(rows)["huge"]
        self.assertLessEqual(len(row["summary"]), 130)
        self.assertTrue(row.get("summary_truncated"))
        self.assertTrue(row["summary"].endswith("…"))

    def test_snapshot_on_demand_matches_the_automatic_broadcast(self):
        server = self.ready()
        _, _, submit_rows = self.submit(server, "a <- 1.5", 5)
        reply = self.snapshot(server, 6)
        self.assertEqual(reply["ev"], "variables")
        self.assertEqual(reply["re"], 6)
        self.assertEqual(self.rowsByName(reply["rows"])["a"], self.rowsByName(submit_rows)["a"])

    def test_snapshot_defaults_what_to_variables(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "snapshot"})   # no "what" field at all
        reply = server.recv()
        self.assertEqual(reply["ev"], "variables")

    def test_snapshot_functions_lists_user_defined_functions_only(self):
        """"User-defined functions (name, signature)" (section 6.4) -- the user workspace's OWN function table,
        not the thousands of built-ins in the global workspace (WorkspaceSnapshot::functionRows)."""
        server = self.ready()
        self.submit(server, "function f(x) { return x + 1 }", 5)

        reply = self.snapshot(server, 6, what="functions")
        self.assertEqual(reply["ev"], "functions")
        names = [row["name"] for row in reply["rows"]]
        self.assertEqual(names, ["f"], reply["rows"])
        self.assertIn("f(x)", reply["rows"][0]["signature"])

    def test_snapshot_all_sends_both_functions_and_variables(self):
        server = self.ready()
        self.submit(server, "a <- 1; function f(x) { return x }", 5)

        server.send({"id": 6, "cmd": "snapshot", "what": "all"})
        seen = {}
        while len(seen) < 2:
            event = server.recv()
            if event.get("re") == 6:
                seen[event["ev"]] = event
        self.assertEqual( [row["name"] for row in seen["functions"]["rows"]], ["f"] )
        self.assertIn( "a", self.rowsByName(seen["variables"]["rows"]) )

    def test_snapshot_what_must_be_a_recognised_kind(self):
        server = self.ready()
        reply = self.snapshot(server, 5, what="bogus")
        self.assertEqual((reply["ev"], reply["code"]), ("error", "bad_request"))

    def test_snapshot_is_rejected_as_busy_while_a_submit_is_running(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "submit", "text": "1+1"})
        server.send({"id": 6, "cmd": "snapshot"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"], reply["re"]), ("error", "busy", 6))

    # --- inspect (S7) -------------------------------------------------------------------------------------------

    def test_inspect_reports_structure_of_a_variable(self):
        """Runs structure(name, verbose=TRUE) (Func_structure.cpp) with its output captured instead of streamed."""
        server = self.ready()
        self.submit(server, "y ~ dnExponential(1)", 5)

        server.send({"id": 6, "cmd": "inspect", "name": "y"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["re"], reply["truncated"]), ("inspection", 6, False))
        self.assertEqual(reply["name"], "y")
        self.assertIn("_RevType", reply["text"])
        self.assertIn("RealPos", reply["text"])
        self.assertIn("_distribution", reply["text"])

    def test_inspect_of_an_unknown_variable_reports_the_error_as_text_not_as_a_protocol_error(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "inspect", "name": "doesNotExist"})
        reply = server.recv()
        self.assertEqual(reply["ev"], "inspection")
        self.assertNotEqual(reply["text"], "")

    def test_inspect_needs_a_string_field_name(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "inspect"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"]), ("error", "bad_request"))

    def test_inspect_summary_is_bounded(self):
        """At most max_inspect_bytes (64 KiB, RevServer.h), with `truncated: true` -- structure() itself still
        builds the whole text internally (Func_structure.cpp has no bound of its own), so this is a small vector,
        not a huge one: only the WIRE size is bounded here, see RevServer::emitOutput's comment."""
        server = self.ready()
        self.submit(server, "x <- rep(1.23456789, 20000)", 5)

        server.send({"id": 6, "cmd": "inspect", "name": "x"})
        reply = server.recv()
        self.assertEqual(reply["ev"], "inspection")
        self.assertTrue(reply["truncated"])
        self.assertLessEqual(len(reply["text"].encode("utf-8")), 64 * 1024)

    # --- complete (S8) ------------------------------------------------------------------------------------------

    def test_complete_offers_matching_function_names(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "complete", "buffer": "mu ~ dnExp"})
        reply = server.recv()
        self.assertEqual(reply["ev"], "completions")
        self.assertEqual(reply["re"], 5)
        self.assertEqual(reply["replace_from"], 5)
        texts = {item["text"] for item in reply["items"]}
        self.assertIn("mu ~ dnExponential", texts)
        kinds = {item["kind"] for item in reply["items"]}
        self.assertEqual(kinds, {"function"})

    def test_complete_cursor_defaults_to_the_end_of_the_buffer(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "complete", "buffer": "dnExp"})
        reply = server.recv()
        texts = {item["text"] for item in reply["items"]}
        self.assertIn("dnExponential", texts)

    def test_complete_respects_an_explicit_cursor_position(self):
        """Only the text up to `cursor` is considered -- matching what linenoise itself does for the terminal
        client, which only ever completes the text before the caret (Completion.h)."""
        server = self.ready()
        # cursor 5 points just after "dnExp"; the rest of the buffer must be ignored
        server.send({"id": 5, "cmd": "complete", "buffer": "dnExpxxxxxxxx", "cursor": 5})
        reply = server.recv()
        texts = {item["text"] for item in reply["items"]}
        self.assertIn("dnExponential", texts)

    def test_complete_needs_a_string_field_buffer(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "complete"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"]), ("error", "bad_request"))

    def test_complete_cursor_must_be_a_non_negative_integer(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "complete", "buffer": "dnExp", "cursor": -1})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"]), ("error", "bad_request"))

    # --- help (S9) -----------------------------------------------------------------------------------------------

    def test_help_renders_a_known_topic(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "help", "topic": "dnNormal"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["re"], reply["topic"], reply["found"]),
                         ("help", 5, "dnNormal", True))
        self.assertIn("dnNormal", reply["text"])

    def test_help_reports_not_found_for_an_unknown_topic(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "help", "topic": "notARealTopicAtAll"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["found"], reply["text"]), ("help", False, ""))

    def test_help_with_an_empty_topic_returns_the_generic_resource_listing(self):
        """Mirrors Func_help.cpp's own default (help() with no argument)."""
        server = self.ready()
        server.send({"id": 5, "cmd": "help", "topic": ""})
        reply = server.recv()
        self.assertTrue(reply["found"])
        self.assertIn("RevBayes help resources", reply["text"])

    def test_help_needs_a_string_field_topic(self):
        server = self.ready()
        server.send({"id": 5, "cmd": "help"})
        reply = server.recv()
        self.assertEqual((reply["ev"], reply["code"]), ("error", "bad_request"))

    # --- busy rules for inspect/complete/help (shared with snapshot's, section 6.3) ----------------------------

    def test_inspect_complete_help_are_rejected_as_busy_while_a_submit_is_running(self):
        """One fresh `submit 1+1` per command, not all three stacked behind a single one: 1+1 finishes so fast
        that by the time the reader thread reached a THIRD follow-up request, the interpreter thread could well
        have already finished and reset `busy` -- found empirically, flaky under exactly that stacking."""
        server = self.ready()
        requests = [
            (16, {"id": 16, "cmd": "inspect", "name": "x"}),
            (17, {"id": 17, "cmd": "complete", "buffer": "dnExp"}),
            (18, {"id": 18, "cmd": "help", "topic": "dnNormal"}),
        ]
        for n, (request_id, request) in enumerate(requests):
            submit_id = 20 + n
            server.send({"id": submit_id, "cmd": "submit", "text": "1+1"})
            server.send(request)
            reply = server.recv()
            self.assertEqual((reply["ev"], reply["code"], reply["re"]), ("error", "busy", request_id), request)
            # drain the submit's own done + the automatic variables broadcast before the next iteration
            while True:
                event = server.recv()
                if event.get("ev") == "done" and event.get("re") == submit_id:
                    self.assertEqual(event["status"], "ok")
                    break
            server.recv()

    # --- protocol 1 commands that phase 1 does not implement yet ------------------------------------------------

    def test_set_is_not_implemented_yet(self):
        """The only protocol-1 command phase 1 leaves unimplemented; every other one now has real coverage above
        (submit/snapshot since S4/S6, ask/answer since S5, inspect/complete/help since S7-S9)."""
        server = self.ready()
        server.send({"id": 10, "cmd": "set"})
        reply = server.recv()
        self.assertEqual((reply.get("re"), reply["ev"], reply["code"]), (10, "error", "not_implemented"))


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
