# Conformance tests for `rb --server`

`run_conformance.py` checks that `rb --server` speaks protocol 1 (newline-delimited JSON on stdin/stdout). It uses only
the Python standard library.

```sh
python3 tests/server/run_conformance.py build/rb          # all tests
python3 tests/server/run_conformance.py build/rb -v -k hello
RB_WRAPPER=wine python3 tests/server/run_conformance.py path/to/rb.exe   # a Windows build under wine
meson test -C build --suite server                        # the same suite through meson
```

The protocol itself is specified in section 6.4 of `GUI_Implementation_Note.md` (repository root); the tests are its
executable form.

## What phase 0 covers

The current backend is a **stub** (`src/revlanguage/ui/RevServer.*`): framing, channel takeover, and the commands
`hello`, `ping`, `interrupt`, `shutdown`. The suite checks the command line (`--server-info`, refusal of incompatible
flags), the handshake (including a non-ASCII working directory and protocol negotiation), error handling for malformed
input, end-of-input behaviour, and that every other protocol-1 command gets a reply carrying its request id.

## What phase 1 adds (not written yet)

Tests to add together with the features they need:

* `submit`: output events, `done` with status and cwd, continuation (`incomplete`), `variables` after `done`.
* Output capture: `printf`/`system("echo ...")` output never appears on the protocol channel; RBOUT and `std::cout`
  text arrive as `output` events; invalid UTF-8 is replaced, not fatal; the process exits 0 after `shutdown`
  (the `rdbuf` restore pitfall).
* Variables: kinds, `x[1]` nested under `x`, `b <-& a` reported with its real type, bounded summaries for a
  10^6-element vector.
* `inspect`, `complete`, `help`.
* `quit()` gives `quit` then `bye` and exit code 0; a function redefinition gives `ask` and waits for `answer`.
* Busy rules: `error{busy}` for requests other than `interrupt`, `answer`, `ping`, `shutdown` while a command runs.
* `interrupt` stops a running MCMC (phase 3); stdin EOF ends a running command within 10 s.
