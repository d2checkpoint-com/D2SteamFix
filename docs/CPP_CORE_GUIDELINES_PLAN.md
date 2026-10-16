# Review and strengthen C++ Core Guidelines alignment


This ExecPlan is a living document. Maintain Progress, Surprises & Discoveries, Decision Log, and Outcomes & Retrospective as the work proceeds. Follow [AGENTS.md](../AGENTS.md), fetched from newer upstream documentation during the review. No repository PLANS.md was present.

## Purpose / Big Picture


D2SteamFix temporarily changes Steam overlay permissions and restores them when Destiny 2 exits. This review preserves that behavior while making ownership, byte-buffer validation, exception boundaries, and build checks follow the applicable C++ Core Guidelines. The outcome is a version 1.1.1 pull request with review coverage, explicit Windows API exceptions, regression checks, and honest validation limits. A review cannot establish the absence of every possible defect or prove the full lifetime safety profile.

## Progress


- [x] (2026-10-16) Read the C++ Core Guidelines skill and inventoried all tracked project files.
- [x] (2026-10-16) Assigned independent owners for ACL recovery, elevation, runtime operations, and build checks; coordinator owns application and shared support code.
- [x] (2026-10-16) Complete review and fixes across every C++ source/header and build configuration.
- [x] (2026-10-16) Integrate six real-Windows fixture test suites, analyzer checks, and complete-source coverage reporting; all production and test executables cross-compile and link with MinGW warnings as errors.
- [ ] Run supported MSVC Debug/Release builds, Windows regression suites, and production coverage in CI.
- [ ] Record exceptions and validation limits, commit version 1.1.1, and open a pull request.

## Surprises & Discoveries


The skill main file is available, but its referenced rule index and guideline shards fail to load through the skill provider. The official isocpp CppCoreGuidelines document was downloaded to /tmp/CppCoreGuidelines.md for targeted rule lookup. This is a current upstream reference, rather than a verified copy of the skill's June 2026 snapshot.

The execution host is Linux and initially offers g++ but no Windows/MSVC SDK, MinGW, Wine, CMake, or clang-tidy. A MinGW GCC 14 toolchain and CMake were acquired under /tmp for supplementary compilation. All production sources, six test suites, an inventory executable, and an overlay fixture DLL compile and link. Supported MSVC builds and Windows execution still need CI; live administrator-prompt behavior needs manual Windows verification.

Initial review identified copyable objects with pointers into their own storage, missing explicit special-member policies, unchecked binary extents, a temporary-journal cleanup ordering issue, and implicit DWORD-to-int exit-status conversion.

## Decision Log


On 2026-10-16, choose a patch release (1.1.1), because the changes correct implementation and validation without adding a user feature. The user explicitly requested a version bump and pull request if changes are made.

On 2026-10-16, preserve the existing Windows application, permission journal format, and launch/repair behavior. Encapsulate necessary C ABI operations under I.30 instead of introducing an unrelated platform or dependency migration. Windows handles are owned only by dedicated resource wrappers.

On 2026-10-16, split writers by file ownership in the shared checkout. ACL owner edits acl.cpp/acl.hpp; elevation owner edits elevation.cpp/elevation.hpp; runtime owner edits process, repair, steam, and overlay; build owner edits CMake, workflows, make.cmd, and new tests. Coordinator integrates shared win32 support, entry handling, command-line/path/log/UI code, and documentation.

On 2026-10-16, the user requested independent cross-review after implementation and test coverage as close to 100% as possible. Expand tests to actual production implementation files and real Windows fixtures. Include every production translation unit in the coverage denominator using an unoptimized inventory executable, fail incomplete reports, and expose uncovered lines. Avoid inventing a percentage before Windows execution or excluding uncalled code to inflate coverage.

## Outcomes & Retrospective


Production review and corrective implementation are complete. Six suites now cover command-line quoting, resource/conversion helpers, application sessions, ACL/recovery, elevation/IPC, and runtime discovery/repair helpers. All binaries cross-compile and link with MinGW warnings as errors. Independent cross-review found and resolved exception-copy safety and an overly broad test deny mask. Supported Windows CI results and remaining coverage gaps will be recorded before delivery.

## Context and Orientation


src/app.cpp validates the Steam invocation and holds the permission protection session while the game runs. src/acl.cpp reads and writes Windows access control lists (ACLs: file permission records) and the recovery journal. src/elevation.cpp requests administrator operations through a helper and named pipe (a Windows process communication channel). src/process.cpp watches verified game processes; src/repair.cpp performs the explicit Steam repair flow. include/steamfix/win32.hpp contains wrappers that release Windows resources automatically. CMakeLists.txt selects C++20 and MSVC; .github/workflows/build.yml builds Windows configurations and release.yml publishes tagged releases. No release tag is created by this task.

## Plan of Work and Milestones


First, read every tracked source/header and supporting build/documentation file. Each owner checks applicable rules, including their reasons and exceptions, and reports findings with rule IDs. Acceptance for this milestone is complete file coverage and concrete findings rather than pattern matching alone.

Second, fix ownership and lifetime hazards, use bounded views for buffer operations, constrain serialization to appropriate types, preserve Windows exit-code bits explicitly, move inherited launch context out of global state, and ensure exceptions cannot escape the Windows entry point. Keep recovery compatibility and security checks intact. Add focused tests for stable behaviors, including command-line quoting. Acceptance is integrated changes with documented API boundaries and no accidental behavior or journal format changes.

Third, enable practical compiler analysis in the Windows build, run portable checks locally, and obtain additional Windows compiler evidence if available. Run the Windows CTest suite in Debug and Release in CI. Record which checks ran versus which are configured or pending. Review the final diff, bump version 1.1.1, commit on a dedicated branch, push, and open a pull request targeting the repository default branch.

## Concrete Steps


From /workspace/D2SteamFix, inspect tracked files using git ls-files and rg. Use rg against /tmp/CppCoreGuidelines.md and read only relevant rule sections. Review source changes with git diff --check and git diff.

Portable command-line expectations pass with g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/command_line_tests.cpp src/command_line.cpp -o /tmp/d2steamfix-command-line-tests followed by running that executable.

On a 64-bit Windows Visual Studio Developer command prompt, run make.cmd test, make.cmd analyze, and make.cmd coverage. Coverage requires OpenCppCoverage 0.9.9.0 and Python. CI builds Debug/Release with STEAMFIX_ENABLE_CODE_ANALYSIS=ON, runs all six suites, and collects Debug coverage. Reports are written under build/coverage in CI; make.cmd coverage writes build-coverage/coverage locally. See docs/BUILDING.md for complete direct CMake commands.

## Validation and Acceptance


Quoting tests must cover empty arguments, whitespace, embedded quotes, backslash runs before quotes, trailing backslashes, and multiple appended arguments. Where available, Windows CommandLineToArgvW must recover the original arguments. Resource and conversion tests should validate ownership transfer and cleanup behavior without modifying real Steam/game files.

Debug and Release Windows builds must preserve /W4 /WX and existing hardening. Analyzer findings must be addressed or narrowly justified at the API boundary. A manual Windows launch/repair test must be reported as pending if no Windows runtime is available; compiling or parsing is not evidence of successful UAC or ACL recovery behavior.

## Idempotence and Recovery


All local changes remain in a dedicated branch for review. Tests use temporary data and do not operate on Steam or game files. Preserve existing recovery files and user work. Do not merge, tag, publish a release, or run real repair operations. Repeat read-only checks safely; reconcile remote branch and pull-request state before retrying an uncertain write.

## Artifacts and Notes


The review record will be docs/CPP_CORE_GUIDELINES.md. It will describe file coverage, corrected findings with rule IDs, deliberate C API exceptions, checks, and remaining limitations. The version derives from CMakeLists.txt, including generated Windows version resources and log version text.

## Interfaces and Dependencies


Use C++20 standard facilities, including std::span, std::array, constrained templates, and std::bit_cast where they express existing requirements. Keep existing Windows libraries and APIs. steamfix::windows_exit_status(DWORD) will preserve Windows process status bits as the existing signed application return type. elevation::accept_elevated_launch will return an optional Steam directory, which app.cpp passes explicitly to its launch flow.

Revision note (2026-10-16): Updated after production fixes, expanded testing, supplementary cross-compilation, and independent cross-review; supported Windows execution and coverage are the remaining validation milestone.
