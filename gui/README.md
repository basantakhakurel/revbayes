# RevStudio (Qt 6 front end for RevBayes)

RevStudio is a graphical front end for RevBayes. It never links the interpreter: it starts `rb --server` as a separate
process and talks to it over a pipe (newline-delimited JSON; protocol in `GUI_Implementation_Note.md`, section 6.4, at
the repository root). This directory is an independent CMake project, built and released separately from `rb`.

**Status: phase 2 (GUI MVP) done, G1-G7; phase 3 underway.** The roadmap and design are in
`GUI_Implementation_Note.md`, sections 6 and 12; section/task numbers below (G*, Q*, C*) are its own.

Phase 0 and phase 1 (the full `rb --server` backend) are done. Phase 2: a dock-based `MainWindow` with persisted
layout, `Settings`/`Theme` (G1); `ConsoleWidget` (G3: output coalescing, history with D9 fixed, continuation,
busy state, the `ask` dialog, a "Backend" tab) in its own dock; the Variables dock (G4: diffing two-level tree,
filters, `inspect`-fed details, Functions tab); the Files dock (G5: rooted at the backend's cwd, actions,
drag-and-drop, `RevString::quote()` fixing D15; double-click opens a file in the editor); `EditorTabs` (G6) --
tabs, open/save/save-as, dirty-state close prompts (also on a backend `quit`), recent files, session restore
(reopened file list, not unsaved content), external-change detection, 4-space auto-indent, `Ctrl+/` comments --
the central widget per the design's own mockup; `PreferencesDialog` (G7: theme, editor/console font, `rb` path,
the file explorer's "watch for changes" -- "Hidden files" persists its own state directly), applied live by
`MainWindow::openPreferences()` except the backend path, which (like `--rb`) only takes effect next launch.

Phase 3, pulled forward on request ahead of the rest of that phase:
- **Q4 `RevHighlighter`**: syntax highlighting, keyword/operator sets from `src/grammar/lex.l`, adapted from the
  MIT-licensed VSCode extension at `/home/basanta/Code/revSyntax`.
- **Q2 Ctrl+C/Stop**: sends `interrupt`; if the backend has not gone back to Ready within 5 s, offers to kill and
  restart it (`Session::restart()`, which needed `Session::start()` to support being called again -- it used to
  work only once per object). Cannot yet halt a long analysis (MCMC, etc.) gracefully: that needs the core
  interrupt flag below (C4/Q1), so a stuck computation is only recoverable by kill-and-restart, which loses the
  Rev session but not open editor tabs.
- **Q7 statement-aware Ctrl+Enter** (RStudio/VSCode-R style): a buffer-wide scan of `(){}[]` depth, skipping
  strings and comments, finds the complete statement containing the cursor, so a multi-line `for(...) { ... }`
  runs as one submission from any of its lines; the cursor then skips forward past blank/comment lines to the
  next real statement, and a statement that is itself only blanks/comments submits nothing.
- **Q5 line numbers, current-line highlight, bracket matching**: the standard `QPlainTextEdit` "Code Editor"
  pattern (a file-local `LineNumberArea` widget overlaid in the viewport margin) plus the same string/comment-
  skipping scan Q7 uses (shared via a small `scanCodeCharacters()` helper), extended to mask out bracket
  characters inside either.
- **Q6 find/replace**: an in-editor bar (`QTextDocument::find`, case/whole-word/regex, wrap-around, Replace/
  Replace All), floated over the editor rather than a dock or dialog -- Ctrl+F finds, Ctrl+H adds the replace
  row, Escape closes it.
- **Q3 console completion and signature tooltip**: Tab (idle only) sends `complete` and shows the results in a
  `QCompleter` popup anchored at the caret -- the manual "QCompleter on a plain text edit" wiring from Qt's own
  Custom Completer example, since QPlainTextEdit has no `setCompleter()` of its own; a single match inserts
  directly with no popup. A signature tooltip on `(` reads from a name-to-signature map filled passively from
  whatever `functions` snapshot arrives on `Session::functionsChanged` (VariablesPanel already requests one on
  ready(); this does not request a second, redundant one of its own).
- **Q7 error links** (the rest of Q7; persisted history was already done in G3): a `source()` failure prints
  `Problem processing line N in file "F"` (RevClient.cpp; `F`'s quoting/escaping is `std::filesystem::path`'s own
  stream insertion, verified against the real `rb`, not just the source), and that substring, wherever it lands
  in the console output, becomes a real clickable link (`QTextCharFormat` anchor) that opens the file at that
  line. `QPlainTextEdit` has no built-in link-click handling (unlike `QTextBrowser`, which risks the large-output
  performance the console's 100000-block cap exists for), so a small `ConsoleOutputEdit` replicates just the
  part needed: `cursorForPosition()` plus the format's own `isAnchor()`/`anchorHref()`.
- **Q8 help dock**: the backend's `help` reply for a topic, plain and monospaced in a `QTextBrowser`, with a
  topic box and back/forward history tracked manually (ordinary browser-history semantics -- a `QStringList`
  plus an index -- rather than through `QTextBrowser`'s own `setSource()`/`loadResource()`, which assumes
  synchronous resource loading and does not fit a round trip over the protocol pipe); F1 (`ScriptEditor::
  wordUnderCursor()`, Rev's own identifier rule, not Qt's generic "word") looks up the backend's help for
  whatever name is under the caret. Verified against the real `rb`'s actual `help` reply, not just the protocol
  doc. Scoped to the editor only, not the console input too -- see `MainWindow`'s own comment on why.

Still phase 3, not done: the core interrupt flag (C4/Q1, so Stop can halt an MCMC run gracefully instead of
killing the process) -- the last item. Session restore also does not reconstruct unsaved buffer content in a
crash (just which files were open; out of scope since G6).

## Layout

```
CMakeLists.txt, CMakePresets.json
src/backend/     ProtocolCodec (framing + JSON), BackendLocator (find rb), BackendProcess (QProcess), Session (state machine)
src/app/         MainWindow, SelfTest, version header template
src/main.cpp     command line: --rb, --cwd, --selftest, --report, --version, --help
resources/       Qt resources (SVG icon)
tests/           QtTest suites, the mock backend (mock-rb), transcripts, self-test runner script
packaging/       linux/ (linuxdeploy scripts, .desktop, icon), windows/ (PowerShell scripts, .rc, .ico), notices
```

`revstudio_core` (backend/) uses QtCore only, so it is testable without a display.

## Build and test on Linux

Qt 6.4 or newer, CMake 3.21 or newer, Ninja. On Ubuntu:

```sh
sudo apt install qt6-base-dev qt6-svg-dev libgl1-mesa-dev ninja-build cmake
cd gui
cmake --preset linux-debug            # system Qt, Debug, ASan + UBSan; also exports compile_commands.json for clangd
cmake --build --preset linux-debug
ASAN_OPTIONS=detect_leaks=0 ctest --preset linux-debug     # headless (QT_QPA_PLATFORM=offscreen is set by the preset)
```

Run it (needs a backend; until `rb --server` is complete, use the mock or a build of `rb` that has `--server`). The
`linux-debug` preset is a Debug+ASan/UBSan build (line 32 above), so running it directly — as opposed to through
`ctest`, which sets this for you — needs the same `ASAN_OPTIONS=detect_leaks=0` as the test command, otherwise
LeakSanitizer dumps a long, harmless report of Qt/GTK3 platform-theme startup allocations (fontconfig/pango/cairo
caches kept alive for the process lifetime — nothing in this project's own code) and flips the exit code to 1 even
though the self-test itself passed. Use `linux-release` instead if you don't want to think about this:

```sh
ASAN_OPTIONS=detect_leaks=0 build/linux-debug/RevStudio --rb build/linux-debug/tests/mock-rb      # or: REVBAYES_EXECUTABLE=/path/to/rb build/linux-debug/RevStudio
ASAN_OPTIONS=detect_leaks=0 build/linux-debug/RevStudio --selftest --rb build/linux-debug/tests/mock-rb
```

`--selftest` runs the real start-up path (icon, locate backend, probe, handshake, ping, submit, variables, shutdown)
and exits 0 or 1; `--report FILE` also writes the report to a file. The backend is found in this order: `--rb`
(then it is the only candidate), the saved setting, `REVBAYES_EXECUTABLE`, next to the GUI executable, `../bin`, `PATH`.

## The mock backend

`mock-rb` speaks the protocol without an interpreter. Options (also read from the environment variable
`MOCK_RB_OPTIONS`): `--stub` (behave like the phase 0 `rb --server`: `submit` answers `not_implemented`),
`--hello-delay-ms`, `--hello-protocol`, `--crash-after N`, `--malformed-after N` / `--malformed-count M`, and
`--transcript FILE` (replay a JSON-lines conversation, see `tests/transcripts/basic.jsonl`).

## Qt versions

| Version | Role | Checked |
|---|---|---|
| 6.4.3 | source floor (Ubuntu 24.04's distro Qt is 6.4.x) | built and all tests passed (phase 0) |
| 6.8.3 | the pinned release build (`QT_VERSION` in `.github/workflows/gui-build.yml`) | built, tested, deployed, smoke-tested (phase 0) |
| 6.11.2 | developer machines with a current distro | built and tested (phase 0) |

The pin is 6.8.3 rather than 6.11.x because, with aqtinstall 3.3.0 (what `install-qt-action` uses), Qt 6.11.x cannot be
installed for Windows. Details in the comment above `QT_VERSION`.

## Packaging (Linux)

```sh
export QT_ROOT_DIR=/path/to/Qt/6.8.3/gcc_64                    # the Qt to bundle (CI: set by install-qt-action)
cmake --preset ci-linux-release && cmake --build --preset ci-linux-release
packaging/linux/make-appdir.sh build/ci-linux-release /tmp/AppDir
packaging/linux/smoke-appdir.sh /tmp/AppDir "$PWD/build/ci-linux-release/tests/mock-rb"
```

`make-appdir.sh` downloads pinned, checksum-verified `linuxdeploy` tools. `smoke-appdir.sh` runs the deployed
RevStudio under `env -i` on the real xcb platform (Xvfb), so a library or plugin missing from the bundle cannot be hidden
by the build machine's Qt. Things learned in phase 0 about the bundle: about 73 MB; `RUNPATH $ORIGIN/../lib`; a
`usr/bin/qt.conf` is written; `libxcb-cursor` **is** bundled; only the `xcb` platform plugin is deployed (no
`offscreen`, so a deployed bundle cannot be tested headlessly with offscreen; use Xvfb).

## Windows (not yet run)

`packaging/windows/make-folder.ps1`, `smoke-folder.ps1`, `revstudio.rc` and the Windows leg of `gui-build.yml` were
written without access to Windows. The `windeployqt` flags were verified against the real tool under wine; nothing else
has been run. **The first CI run is their first test.** Things to look at first:

- does the deployed folder start on a clean machine (MSVC runtime, `platforms/qwindows.dll`, SVG plugins)?
- is a console window shown when the GUI starts `rb.exe`? (Qt should suppress it for a GUI parent; if not, use
  `QProcess::setCreateProcessArgumentsModifier` with `CREATE_NO_WINDOW`.)
- `RevStudio.exe` is a GUI-subsystem program. `smoke-folder.ps1` therefore uses `Start-Process -Wait -PassThru` and the
  `--report` file, and the ctest self-tests read the report file rather than captured stdout.
- non-ASCII user and directory names.

## CI

`.github/workflows/gui.yml` (pull requests and pushes that touch `gui/`) runs a distro-Qt leg (Ubuntu 24.04, sanitizers)
and calls `gui-build.yml`, the reusable workflow that builds, tests, deploys and smoke-tests with the pinned Qt on
Ubuntu 22.04 and Windows (MSVC 2022). Release integration (bundling with `rb`) is phase 4.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `qt.svg: Cannot open file ':/icons/...'` | the `.qrc` was not compiled: `qt_standard_project_setup()` does not enable AUTORCC (the project sets it) |
| "Could not find the Qt platform plugin offscreen" in a deployed bundle | only `xcb` is deployed; test a bundle with Xvfb, not offscreen |
| `actions/upload-artifact` output has lost its executable bits | it drops permissions and symlinks: upload a tarball |
| a GUI-subsystem `.exe` "returns immediately" in PowerShell | use `Start-Process -Wait -PassThru` |
