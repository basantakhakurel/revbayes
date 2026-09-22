# RevStudio (Qt 6 front end for RevBayes)

RevStudio is a graphical front end for RevBayes. It never links the interpreter: it starts `rb --server` as a separate
process and talks to it over a pipe (newline-delimited JSON; protocol in `GUI_Implementation_Note.md`, section 6.4, at
the repository root). This directory is an independent CMake project, built and released separately from `rb`.

**Status: phase 0 (spike and foundations).** A skeleton window, the backend process supervision, the protocol client,
a mock backend, tests, CI workflows and packaging scripts. The real interface (editor, console, variables panel, file
explorer) is phase 2. The roadmap and design are in `GUI_Implementation_Note.md`, sections 6 and 12.

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

Run it (needs a backend; until `rb --server` is complete, use the mock or a build of `rb` that has `--server`):

```sh
build/linux-debug/RevStudio --rb build/linux-debug/tests/mock-rb      # or: REVBAYES_EXECUTABLE=/path/to/rb build/linux-debug/RevStudio
build/linux-debug/RevStudio --selftest --rb build/linux-debug/tests/mock-rb
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
