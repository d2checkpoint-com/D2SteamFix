# Building from Source

## Requirements

- 64-bit Windows.
- Visual Studio or Visual Studio Build Tools with the **Desktop development with C++** workload, including MSVC and a Windows SDK. CI uses Visual Studio 2022.
- CMake 3.24 or newer, available on `PATH`. The CMake tools included with Visual Studio can be used.

The project uses C++20 and requires MSVC. Linux, macOS, and non-MSVC builds are not supported.

## Build

Open a Visual Studio Developer Command Prompt in the repository root and run:

```batch
make.cmd configure
make.cmd build
```

The Release executable is written to `build\Release\steamfix.exe`. The `build` task also configures the build tree, so the separate `configure` step is optional.

For a Debug build:

```batch
make.cmd debug
```

The Debug executable is written to `build\Debug\steamfix.exe`.

To build and run the regression tests in Debug:

```batch
make.cmd test
```

To build with Microsoft C++ Core Check and run those tests:

```batch
make.cmd analyze
```

The `analyze` task uses a separate `build-analysis` directory. The checked-in
[ruleset](../cmake/CoreGuidelines.ruleset) treats the selected resource ownership,
special-member, initialization, and lifetime diagnostics as errors. This is a
focused enforcement baseline, not an automated check of every Core Guideline.

To remove the `build`, `build-analysis`, and `build-coverage` directories:

```batch
make.cmd clean
```

This preserves runtime logs and recovery files. Do not delete `steamfix.dat` from a game directory to clean a build; it may be needed to restore Steam overlay permissions.

## Direct CMake Commands

To use the same generator and architecture as CI:

```batch
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DSTEAMFIX_ENABLE_CODE_ANALYSIS=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure --no-tests=error
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

Analysis requires a Visual Studio generator. To run the complete set of currently
implemented Microsoft Core Check rules for additional review, configure with
`-DSTEAMFIX_CODE_ANALYSIS_RULESET=CppCoreCheckRules.ruleset`. The complete ruleset
can produce additional diagnostics for Win32 API interoperability and is not the
CI baseline. Set `-DBUILD_TESTING=OFF` when only the application is needed.

## Coverage

Install the 64-bit [OpenCppCoverage](https://github.com/OpenCppCoverage/OpenCppCoverage)
tool and Python 3, then run:

```batch
make.cmd coverage
```

This builds Debug in `build-coverage` and runs the tests under OpenCppCoverage.
The results are written to `build-coverage\coverage`: `html\index.html` shows
annotated sources, `coverage.xml` contains Cobertura data, and `summary.md` gives
the measured total, per-file coverage, and every uncovered executable line range.
`LastTest.log` retains output from passing suites too, including any conditional
privilege or filesystem skips; `LastCoverageResults.log` records the collector.
The Debug CI job publishes the same files as the
`steamfix-windows-x64-coverage` artifact and adds the summary to the workflow run.

The coverage inventory executable loads Debug symbols for every production
translation unit, including `main.cpp` and the real dialog implementation. It
performs no test itself. OpenCppCoverage combines its unexecuted lines with
coverage from the tests and their child processes, so completely uncalled code
still contributes to the denominator. The summary rejects a report that omits a
production `.cpp` file. No production lines are excluded to improve the score,
and no arbitrary coverage threshold is substituted for a measured result.

Line coverage counts executable lines described by Debug symbols. It does not
measure branch coverage, comments, declarations, or uninstantiated templates,
and a high percentage does not establish correctness of real Steam or UAC flows.

## Validation

Build both Debug and Release when changing code. The compiler treats warnings as errors (`/W4 /WX`), and both configurations must compile successfully.

[GitHub Actions](../.github/workflows/build.yml) builds both configurations with
the focused C++ Core Check ruleset and runs CTest on Windows x64 for pushes and
pull requests. The executables are available as workflow artifacts named
`steamfix-windows-x64-Debug` and `steamfix-windows-x64-Release`.

The tests use real Windows APIs with files, processes, named pipes, security
descriptors, and registry fixtures owned by the test. They cover:

- Command-line quoting, Win32 handle ownership, text conversion, and exit-status bits.
- Invocation validation, environment handling, logging, and application launch,
  monitoring, permission restoration, and launch-failure cleanup with fixture executables.
- Malformed ACL and recovery-journal inputs, DPAPI round trips, and restoration
  of permissions on fixture files.
- Elevation helper validation, wire formats, overlapped pipe I/O, cancellation,
  and temporary privilege restoration without requesting UAC.
- Broker startup, authentication, reuse, recovery after transport loss, and
  administrator handoff through a scoped shell substitute that starts only the
  same test executable. These cases require an already elevated test token;
  actual UAC consent and alternate-account behavior remain manual checks.
- Path and Steam-directory validation, process discovery and termination of
  fixture children, overlay-module identification, and repair helper behavior.
- Recovery from finite process-observation failures while monitoring real
  fixture children. This separate test target injects failures at specific API
  call sites without changing the production application.
- Repair orchestration with a fixture Steam executable, including consent,
  selection, launch, verification, and recovery-cleanup failures. These tests
  constrain discovery and process snapshots to fixture entries and supply
  deterministic consent/selection responses; file, token, and process operations
  act on real test-owned resources.
- Real information/error dialogs and the built wrapper's invalid-invocation
  entry point, with dialogs dismissed only in child processes owned by the test.

Application tests replace dialogs with headless sinks; the real dialog code is
also exercised by the separate UI test when a Windows desktop is available.
An independent native window probe verifies desktop access before any test child
is launched. Only recognized desktop-access failures in that probe report a
CTest skip (exit 77); a missing or hung dialog always fails, including the first
information dialog. Owned children that exit without a window or remain idle
verify both failure paths. The suite does not launch an installed Steam client, change
an installed game's permissions, request UAC, or perform a live Steam repair.
See [AGENTS.md](../AGENTS.md) for the affected paths that still need manual
Windows checks. Use the [README](../README.md) for installation and normal use.
