# Repository Guidance

## Project and Documentation

D2SteamFix is a Windows x64 C++20 launch wrapper for the Steam version of Destiny 2 (Steam App ID `1085660`). It temporarily denies execute access to Steam's overlay renderer DLLs, launches the game, waits for the session to end, and restores the original permissions. Direct invocation offers a separate Steam repair flow.

- Keep [README.md](README.md) focused on installation, use, recovery, removal, and troubleshooting.
- Keep build requirements and commands in [docs/BUILDING.md](docs/BUILDING.md).
- Keep [docs/EXIT_CODES.md](docs/EXIT_CODES.md) aligned with `ExitCode` in `include/steamfix/error.hpp`.
- `CLAUDE.md` is a relative symlink to `AGENTS.md`. Preserve the symlink and maintain these instructions in one place.

## Code Map

- `src/main.cpp`: Windows entry point; delegates to `src/app.cpp` for invocation validation and session orchestration.
- `src/acl.cpp`: overlay permission changes, recovery journal, and restoration.
- `src/repair.cpp`: consent, process shutdown, Steam overlay replacement, verification, and recovery cleanup.
- `src/steam.cpp`, `src/paths.cpp`, `src/process.cpp`: Steam discovery, path validation, and process tracking.
- `src/elevation.cpp`, `src/command_line.cpp`: administrator relaunch, desktop-user launches, and Windows argument quoting.
- `src/logging.cpp`, `src/ui.cpp`: diagnostics and Windows dialogs.
- `include/steamfix/`: module interfaces, application errors, and Win32 resource helpers.
- `CMakeLists.txt`, `make.cmd`, `.github/workflows/build.yml`: build configuration, local tasks, and Windows CI.

## Conventions

- Follow the existing four-space indentation, `steamfix` namespaces, and source/header organization.
- Use Unicode Win32 APIs and `std::filesystem::path` for Windows paths. Preserve command-line argument quoting and support for paths containing spaces.
- Use RAII for handles and allocated resources; reuse helpers such as `UniqueHandle` and `LocalMemory` in `include/steamfix/win32.hpp`.
- Preserve stage-specific `AppError` reporting and useful logs when adding failure paths.
- Keep the MSVC warning and security settings in `CMakeLists.txt`; fix warnings rather than weakening `/W4 /WX`.

## Behavior to Preserve

- Normal launches change overlay DLL permissions only. Do not introduce code injection, process-memory inspection, game-file changes, Steam configuration changes, or anti-cheat bypasses.
- Validate the Destiny launcher, Steam App IDs, paths, and process identity before acting. Retain the single-session guard and checks for an existing game session or an overlay already loaded into the wrapper.
- Preserve the original permissions in the recovery journal before applying changes. Reuse valid interrupted-session recovery state and preserve mismatched or unusable backups for recovery or repair.
- Store `steamfix.dat` and logs in the game directory determined from the forwarded launcher path, independent of the working directory. Do not discard recovery state during build cleanup.
- Restore permissions on normal exit and launch failures. Keep elevation retry and cancellation paths consistent with recovery; request elevation only when needed.
- Repair requires consent before closing applications or replacing Steam overlay files. Verify repaired files before deleting recovery state, preserve logs, and retain support for installations without the optional 32-bit renderer DLL.

## Validation

Follow [docs/BUILDING.md](docs/BUILDING.md) and build both Debug and Release on Windows x64 with MSVC. CI runs those builds and the automated tests for pushes and pull requests. Tests use controlled fixtures; do not claim live Steam, desktop, UAC, or repair validation from a successful compile or unit-test run.

For changes to launch, permissions, elevation, or repair, manually check the affected paths on a Windows test installation:

- Steam launch with a quoted path containing spaces, including a different working directory.
- Normal game exit and restoration of the original overlay permissions.
- Launch failure, administrator retry, and cancellation with recovery state preserved or restored as appropriate.
- Interrupted-session recovery, unusable backups, and blocked overlay permissions with no backup.
- Direct-launch repair: cancellation, successful verification and cleanup, and failure without premature recovery cleanup; include an installation without the optional 32-bit DLL when relevant.

For documentation-only changes, check links, command examples against the build scripts, and `git diff --check`. State clearly which builds or manual checks could not be run in the current environment.
