# C++ Core Guidelines review

This review covers the entire D2SteamFix project at version 1.1.1. The reference
is the supplied `cpp-core-guidelines` skill, with detailed rule lookup against
the official [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).
The skill's main instructions loaded, but its separate reference files were
unavailable from the provider. The fallback is current upstream, rather than a
verified copy of the skill's June 2026 snapshot.

The review applies guideline reasons, notes, and exceptions to this Windows x64
C++20 application. It is not a claim that every guideline is mechanically
enforced, that the strict type/bounds/lifetime profiles have been proved, or that
line coverage proves correctness. The Guidelines contain judgment calls and
unfinished rules; a compiler check alone cannot certify complete conformance.

## Review coverage

| Files | Areas checked |
| --- | --- |
| `src/acl.cpp`, `include/steamfix/acl.hpp` | Permission invariants, recovery compatibility, descriptor/ACE/SID extents, serialization, encryption, resource cleanup, restoration on failure |
| `src/elevation.cpp`, `include/steamfix/elevation.hpp` | Authenticated helper protocol, cancellation lifetime, const correctness, privileged operations, handle ownership, launch context, error propagation |
| `src/process.cpp`, `include/steamfix/process.hpp` | Process identity, observation reliability, fail-closed monitoring, wait results, signed arithmetic, exit-status representation |
| `src/repair.cpp`, `include/steamfix/repair.hpp` | Consent boundaries, target verification, token storage, wait failures, file lifetime, optional renderer behavior, cleanup ordering |
| `src/steam.cpp`, `src/paths.cpp`, their headers | Registry bounds, string validity, case-insensitive path comparisons, discovery errors, self-contained includes |
| `src/overlay.cpp`, `include/steamfix/overlay.hpp` | Snapshot lifetime, module enumeration errors, path and name validation |
| `src/app.cpp`, `src/main.cpp`, `include/steamfix/app.hpp` | Invocation validation, protected-session lifetime, error recovery, exception boundary, inherited discovery context |
| `src/command_line.cpp`, `include/steamfix/command_line.hpp` | Windows quoting rules, string ownership, direct includes |
| `src/logging.cpp`, `include/steamfix/logging.hpp` | File invariants, ownership, concurrent writes, lock duration, nonthrowing diagnostics |
| `src/ui.cpp`, `include/steamfix/ui.hpp` | Borrowed text, null termination at the API, nonthrowing fallback |
| `include/steamfix/win32.hpp`, `include/steamfix/error.hpp` | Resource wrappers, special members, Unicode sizes, exception copying, process status conversion |
| Build scripts, both workflows, resource/manifest files, repository guidance and documentation | C++20, warning/security settings, version generation, analysis, testing, coverage, consistency with runtime behavior |
| `tests/`, `cmake/` | Fixture isolation, resource/thread cleanup, test linkage, complete coverage denominator, failure visibility |

## Corrected findings

**C.21, C.37, C.41, C.49, R.1, R.12, E.6, E.16.** Resource and scope guards now
state their complete copy/move policy. Self-referencing security attributes
cannot be copied or moved. User identity owns independent SID bytes instead of
caching a pointer into a token buffer. Permission probes use resource wrappers.
The failed journal writer closes its exclusive temporary handle before deleting
the file. Protection destruction cannot propagate cleanup exceptions; explicit
restoration continues to report failures and preserves recovery state.

**E.16, C.64, C.128.** Application and relaunch exceptions hold immutable shared
context so copying an exception does not allocate. Explicit nonthrowing moves
preserve both objects' accessor invariants. Tests check the type traits and
exception capture/rethrow behavior.

**P.7, ES.65, ES.103, F.24, T.10, Bounds.1/Bounds.3.** ACL readers validate each
ACE's structural bounds and basic allow/deny ACE SID extents before native APIs
inspect the data. Other native ACE forms retain their Windows validation. Byte operations use
validated spans, and binary copying/IPC templates constrain their accepted
types. Permission protection accepts a bounded target view and rejects
duplicate targets before writing a recovery journal. Journal magic values,
layout, encryption, and recovery version remain unchanged.

**I.2, CP.2, CP.20, R.1.** The authenticated Steam directory is returned to the
coordinator and passed explicitly. Administrator privileges are scoped and
restored; process-token changes are serialized to prevent overlapping
save/restore sequences. Log formatting runs outside the write lock, while each
complete message remains serialized.

**ES.46, ES.48, ES.50, SL.con.1, P.4.** Private IPC uses const-correct spans and
explicit wire layout without padding disclosure. Rename lengths are checked
before conversion; the native rename buffer has deterministic padding and an
explicit trailing terminator. Token headers are copied into typed locals, the privilege
record has typed aligned storage, and application-owned fixed buffers use
`std::array`. Windows exit-code bits use an explicit same-width `bit_cast` at the
signed application boundary.

**P.7, E.2, E.12, F.6.** Repair rejects failed or unexpected process waits before
acting. The application has a final nonthrowing exception boundary covering
startup, logging, and even failure-message formatting. Stage-specific errors
and the recovery journal remain available for diagnosis. A missing optional
32-bit renderer is distinguished from a filesystem inspection failure, including
standard-library implementations that also set an error code for a missing path.
Permission probes explicitly reject invalid handles, including the sentinel
whose bit pattern is also Windows' current-process pseudo-handle.

**SF.8, SF.10, SF.11, Con.1.** All project headers use standard component-specific
include guards and compile independently, including when included twice. Headers
and source files include their direct dependencies. New local bindings are immutable where their values do not
change. The project remains C++20 with warnings as errors and the existing
security/linker settings.

## Deliberate Windows API boundaries

I.30 requires containing necessary unsafe operations. Win32 interoperability
still uses `HANDLE`, `HKEY`, `DWORD`, native structures, null-terminated strings,
and borrowed ACL/SID pointers. Their lifetime is held by a resource wrapper or
the surrounding owned storage. Native unsigned fields describe bit masks,
Windows API extents, or the existing serialized format. Signed values are used
for arithmetic counters that are not API extents.

Some Windows permission and encryption APIs accept mutable pointers for input
they do not modify. One documented adaptation in `acl.cpp` isolates this
constness mismatch, following the ES.50 exception for interfaces without
const-correct input types. Runtime byte representation at these C API
boundaries remains local; strict Type/Bounds profiles are not asserted.

The helper sends a duplicated handle into its authenticated parent. If response
delivery fails after duplication, the parent can hold an unreported handle
until it exits. Closing that remote value on timeout can race with successful
adoption and invalidate an in-use or recycled handle. Transport failure drops
the connection and propagates an error; Windows reclaims the handle at parent
exit. This retained failure-path limitation is explicitly not claimed as full
RAII coverage of distributed handle transfer.

The permission broker and privilege mutex have application lifetime, initialized
on first use. They are encapsulated inside the elevation implementation; no
mutable discovery state is exposed. Removing this remaining service lifetime
would require a separate explicit coordinator-context refactor.

## Verification and coverage

`make.cmd test` runs the automated suite. `make.cmd analyze` adds the checked-in
Microsoft C++ Core Check ruleset. It enforces selected ownership, special-member,
initialization, and lifetime diagnostics as errors; it does not check every
guideline. The optional full Microsoft ruleset is described in
[BUILDING.md](BUILDING.md).

Native MSVC analysis exposed limitations in its experimental lifetime model for
engaged optionals and owning aggregates containing filesystem paths. The
remaining ACL diagnostics have statement-specific suppressions with the storage
invariant documented next to each call; independent review confirmed the owners
outlive those uses. Tests also locally suppress moved-from diagnostics where
they intentionally verify an explicitly supported empty or preserved state.
No selected rule is disabled globally. Other reports were resolved by removing
a borrowed recovery-entry pointer, using immutable span views, copying a
timestamp value, and constructing the logger directly in its final scope.

Nine test suites exercise actual production implementations and real Windows APIs on
test-owned directories, registry overrides, pipes, mutexes, module fixtures, and
child processes. They include malformed recovery inputs, DPAPI round trips,
permission restoration and interrupted recovery, transport failures, process
monitoring state, quoted/Unicode arguments, and launch failure cleanup. The
application session test starts copies of the test program under the validated
launcher/game names inside its own temporary tree. Coordinator and repair tests
also supply finite failures at scoped external boundaries to check restoration,
cancellation, shutdown, verification, and recovery preservation. Repair discovery
and consent responses stay within the fixture; native file/token operations and
termination act on owned resources. No installed Steam client or Destiny 2 game
is launched. Application test dialogs use headless sinks; a separate test reads
and dismisses real production dialogs belonging to its own children and tests
the built wrapper's invalid-invocation entry point. An unavailable desktop is
reported as a skip, never as successful UI validation.

Broker startup, reuse, transport-loss recovery, and administrator handoff tests
intercept the shell `runas` boundary and start only the same test executable with
ordinary `CreateProcessW`. These cases require an already elevated token. A
scoped parent-only elevation-report override reaches the relaunch path; helper
children use their actual token, process/pipe identity checks, and handle
duplication. This covers the coordinator protocol without claiming UAC consent
or alternate-account validation.

Independent reviewers checked production changes outside their original file
ownership, then cross-reviewed the integrated test boundaries, native contracts,
cleanup lifetimes, and coverage inventory. Findings were resolved before the
final review; the resulting review reported no actionable regressions. This is
review evidence, rather than a proof of complete guideline conformance.

Debug CI collects OpenCppCoverage HTML and Cobertura reports plus an uncovered
line summary. An inventory executable retains every production translation unit
so code uncalled by tests still contributes to the denominator. Missing source
files make coverage reporting fail. This measures executable-line coverage,
including production headers with emitted code; it is not branch coverage.
Coverage is reported as measured, without excluding difficult code to inflate
the percentage or replacing the user's goal with a lower arbitrary threshold.

Local verification so far: all production translation units and the initial
tests cross-compile as Windows x64 C++20 with MinGW GCC 14 and warnings as errors;
the application links against the Windows libraries. Portable command-line
expectations pass under GCC. This supplements, and does not replace, the
supported MSVC Debug/Release builds and Windows execution. Final CI and expanded
suite results are recorded in the pull request.

Live Steam behavior, UAC consent/cancellation, alternate administrator accounts,
and destructive repair against a real installation require the manual Windows
checks in [AGENTS.md](../AGENTS.md). They are not covered by compiling or by
isolated fixtures. Catastrophic OS/API failures and allocation failures also
leave executable error paths that may remain uncovered.
