# Conformance tests for `rb --server`

`run_conformance.py` checks that `rb --server` speaks protocol 1 (newline-delimited JSON on stdin/stdout). It uses
only the Python standard library.

```sh
python3 tests/server/run_conformance.py build/rb          # all tests
python3 tests/server/run_conformance.py build/rb -v -k hello
RB_WRAPPER=wine python3 tests/server/run_conformance.py path/to/rb.exe   # a Windows build under wine
meson test -C build --suite server                        # the same suite through meson
```

The protocol itself is specified in `doc/server-protocol.md` (repository root `doc/`); the tests are its
executable form. (`GUI_Implementation_Note.md`, also at the repository root, is the design document the backend
was built from -- useful for *why* something works the way it does, not for the wire format itself.)

## What the suite covers

Framing and the handshake (including a non-ASCII working directory, protocol-version negotiation, and command-line
flags like `--server-info` and incompatible-flag refusal); malformed input and end-of-input behaviour; `submit`
(output streaming, continuation, syntax errors, busy rejection, non-ASCII round-tripping, `quit()`); the `ask`/
`answer` exchange for a function redefinition mid-`submit`, including what happens if the front end disappears
while an `ask` is pending; `snapshot` (`variables`, `functions` and `all`, including kinds, `x[1]` nested under
`x`, a reference variable's real value instead of `NULL`, and a bounded summary for a huge container); `inspect`
(including the 64 KiB cap); `complete` (cursor handling, argument-position completion); `help`; a non-ASCII
`setwd()`; and that `interrupt`/`ping` are answered immediately even while a `submit` is running.

## What is not implemented yet

* `set` (replies `not_implemented`).
* Real interruption of a running `submit`: `interrupt` reports whether the interpreter was busy, but cannot
  actually cancel it (needs a core change -- an `std::atomic<bool>` polled inside the long analysis loops).

## Adding a test

`ServerCase` (in `run_conformance.py`) has helpers for the common shapes: `ready()` (hello already done),
`submit()`/`finishSubmit()` (drain a submit's output/done/variables), `snapshot()`, `triggerAsk()`, and
`rowsByName()`. Prefer extending those over hand-rolling the send/recv loop, unless the test is specifically about
something those helpers hide (ordering, a mid-stream event, raw framing).
