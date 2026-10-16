#include "steamfix/app.hpp"

#include "steamfix/acl.hpp"
#include "steamfix/command_line.hpp"
#include "steamfix/error.hpp"
#include "steamfix/elevation.hpp"
#include "steamfix/logging.hpp"
#include "steamfix/overlay.hpp"
#include "steamfix/paths.hpp"
#include "steamfix/process.hpp"
#include "steamfix/repair.hpp"
#include "steamfix/steam.hpp"
#include "steamfix/ui.hpp"
#include "steamfix/win32.hpp"

#include <shellapi.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace steamfix::app {
namespace {

constexpr std::wstring_view invalid_message =
    L"D2SteamFix was not started by Steam with a Destiny 2 command.\n\n"
    L"This program must be run from Steam using launch options:\n"
    L"\"C:\\path\\to\\steamfix.exe\" %command%\n\nDestiny 2 was not started.";

struct Invocation {
    fs::path launcher;
    fs::path game_dir;
    std::vector<std::wstring> args;
};

bool validate_app_ids(const std::optional<std::wstring>& app_id,
                      const std::optional<std::wstring>& game_id) noexcept;
Invocation validate_invocation(const std::vector<std::wstring>& args,
                               const std::optional<std::wstring>& app_id,
                               const std::optional<std::wstring>& game_id);

std::optional<std::wstring> environment(const wchar_t* name) {
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        SetLastError(ERROR_SUCCESS);
        const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
        if (needed == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
            return std::wstring{};
        }
        std::wstring value(needed, L'\0');
        const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
        if (written == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
            return std::wstring{};
        }
        if (written < needed) {
            value.resize(written);
            return value;
        }
    }
    throw std::runtime_error("environment variable changed repeatedly while it was being read");
}

std::wstring optional_id(const std::optional<std::wstring>& value) {
    return value ? L"Some(\"" + *value + L"\")" : L"None";
}

std::vector<std::wstring> command_line_arguments() {
    int count = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (raw == nullptr) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    const LocalMemory guard(raw);
    if (count < 1) throw std::runtime_error("Windows returned an empty command line");
    // CommandLineToArgvW owns one LocalFree block containing count pointers.
    const std::span<wchar_t* const> arguments(raw, static_cast<std::size_t>(count));
    std::vector<std::wstring> result;
    result.reserve(arguments.size() - 1);
    for (const auto argument : arguments.subspan(1)) result.emplace_back(argument);
    return result;
}

fs::path current_executable() {
    std::wstring buffer(32768, L'\0');
    const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    buffer.resize(written);
    return fs::path(buffer);
}

class SessionMutex final {
public:
    explicit SessionMutex(UniqueHandle handle) : handle_(std::move(handle)) {}
    ~SessionMutex() { ReleaseMutex(handle_.get()); }
    SessionMutex(const SessionMutex&) = delete;
    SessionMutex& operator=(const SessionMutex&) = delete;
    SessionMutex(SessionMutex&&) = delete;
    SessionMutex& operator=(SessionMutex&&) = delete;
private:
    UniqueHandle handle_;
};

SessionMutex session_mutex() {
    SetLastError(ERROR_SUCCESS);
    const HANDLE raw = CreateMutexW(nullptr, TRUE, L"Global\\D2SteamFix-1085660");
    if (raw == nullptr) {
        throw AppError(ExitCode::existing_session, "single-instance check", windows_error());
    }
    const bool already_exists = GetLastError() == ERROR_ALREADY_EXISTS;
    UniqueHandle handle(raw);
    if (already_exists) {
        throw AppError(ExitCode::existing_session, "single-instance check",
                       "another D2SteamFix session is already active on this computer");
    }
    return SessionMutex(std::move(handle));
}


void require_regular_file(const fs::path& path, const ExitCode code, const char* stage,
                          const char* missing_detail) {
    std::error_code error;
    const bool regular = fs::is_regular_file(path, error);
    if (error) {
        throw AppError(code, stage, display_path(path) + ": " + error.message());
    }
    if (!regular) throw AppError(code, stage, missing_detail);
}

void check_overlay() {
    try {
        const std::optional<fs::path> loaded = overlay::loaded_renderer();
        if (loaded) {
            throw AppError(ExitCode::overlay_loaded, "self overlay check",
                           "Steam injected " + display_path(*loaded) +
                           " before protection was established");
        }
    } catch (const AppError&) {
        throw;
    } catch (const std::exception& error) {
        throw AppError(ExitCode::overlay_loaded, "self overlay check",
                       "could not safely enumerate wrapper modules: " + std::string(error.what()));
    }
}

struct ChildProcess {
    UniqueHandle process;
    DWORD pid;
};

ChildProcess launch(const Invocation& invocation, const fs::path& steam_directory, const Logger& log) {
    std::wstring command_line;
    command_line::append_quoted(command_line, invocation.launcher.native());
    for (const std::wstring& argument : invocation.args) {
        command_line::append_quoted(command_line, argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    bool created = elevation::launch_as_desktop_user(invocation.launcher, command_line,
                                                    invocation.game_dir, information);
    if (!created) {
        if (elevation::is_elevated()) {
            log.warn("desktop-user launch unavailable; launching from the existing administrator context");
        }
        created = CreateProcessW(invocation.launcher.c_str(), command_line.data(), nullptr, nullptr,
                                  FALSE, 0, nullptr, invocation.game_dir.c_str(), &startup, &information) != FALSE;
    }
    if (!created) {
        const DWORD error = GetLastError();
        if (error != ERROR_ELEVATION_REQUIRED && !elevation::permission_denied(error)) {
            throw AppError(ExitCode::process_creation, "launcher creation", windows_error(error));
        }
        if (elevation::is_elevated()) {
            throw AppError(ExitCode::process_creation, "launcher creation",
                           "Windows still denied the launcher while D2SteamFix was running as administrator: " +
                           windows_error(error));
        }
        log.info("Windows denied launcher creation; restoring permissions before requesting an administrator launch");
        throw elevation::RelaunchRequired(error, invocation.game_dir, invocation.launcher, steam_directory);
    }
    UniqueHandle process(information.hProcess);
    const UniqueHandle thread(information.hThread);
    return {std::move(process), information.dwProcessId};
}

std::string existing_processes(const std::vector<std::pair<DWORD, std::wstring>>& existing) {
    std::ostringstream output;
    output << "verified Destiny processes already running: [";
    std::string_view separator;
    for (const auto& [pid, name] : existing) {
        output << separator << '(' << pid << ", " << narrow(name) << ')';
        separator = ", ";
    }
    output << ']';
    return output.str();
}

int run(const Logger& log, const std::vector<std::wstring>& forwarded, const fs::path& recovery,
        const std::optional<fs::path>& inherited_steam_directory) {
    const std::optional<std::wstring> app_id = environment(L"SteamAppId");
    const std::optional<std::wstring> game_id = environment(L"SteamGameId");
    log.info(narrow(L"inherited App IDs: SteamAppId=" + optional_id(app_id) +
                    L", SteamGameId=" + optional_id(game_id)));
    const Invocation invocation = validate_invocation(forwarded, app_id, game_id);
    log.info("validated launcher path: " + display_path(invocation.launcher));
    log.info("relevant App ID: 1085660");
    fs::path steam_directory;
    try {
        if (inherited_steam_directory) {
            steam_directory = *inherited_steam_directory;
        } else {
            steam_directory = steam::discover();
        }
    } catch (const std::exception& error) {
        throw AppError(ExitCode::steam_discovery, "Steam discovery", error.what());
    }
    log.info("Steam directory: " + display_path(steam_directory));
    std::error_code recovery_error;
    const bool has_recovery = fs::exists(recovery, recovery_error);
    if (recovery_error) {
        throw AppError(ExitCode::overlay_protection, "ACL recovery lookup", recovery_error.message());
    }
    if (!has_recovery && repair::needed(steam_directory, log)) {
        log.warn("renderer execute access is blocked without a recovery backup; offering Steam repair");
        static_cast<void>(repair::run(invocation.game_dir, recovery, log));
        return 0;
    }
    check_overlay();
    std::vector<std::pair<DWORD, std::wstring>> existing;
    try {
        existing = process::matching_in_dir(invocation.game_dir);
    } catch (const std::exception& error) {
        throw AppError(ExitCode::existing_session, "existing-session check", error.what());
    }
    if (!existing.empty()) {
        throw AppError(ExitCode::existing_session, "existing-session check", existing_processes(existing));
    }
    const fs::path renderer64 = steam_directory / L"GameOverlayRenderer64.dll";
    require_regular_file(renderer64, ExitCode::steam_discovery, "renderer discovery",
                         "GameOverlayRenderer64.dll is missing or is not a regular file");
    std::vector<fs::path> targets{renderer64};
    log.info("selected renderer DLL: " + display_path(renderer64));
    const fs::path renderer32 = steam_directory / L"GameOverlayRenderer.dll";
    std::error_code renderer32_error;
    const fs::file_status renderer32_status = fs::status(renderer32, renderer32_error);
    if (renderer32_error && renderer32_status.type() != fs::file_type::not_found) {
        throw AppError(ExitCode::steam_discovery, "renderer discovery",
                       display_path(renderer32) + ": " + renderer32_error.message());
    }
    if (fs::is_regular_file(renderer32_status)) {
        log.info("selected renderer DLL: " + display_path(renderer32));
        targets.push_back(renderer32);
    } else {
        log.warn("optional GameOverlayRenderer.dll is absent");
    }

    acl::Protection protection = acl::Protection::apply(targets, recovery, log);
    log.info("renderer execute denial established for the current user");
    bool launcher_started = false;
    std::exception_ptr session_error;
    std::optional<int> exit_status;
    try {
        check_overlay();
        ChildProcess child = launch(invocation, steam_directory, log);
        launcher_started = true;
        log.info("launched destiny2launcher.exe; beginning fail-closed process monitoring");
        exit_status = process::monitor(child.process.get(), child.pid, invocation.game_dir, log);
    } catch (...) {
        session_error = std::current_exception();
    }

    try {
        protection.restore(log);
    } catch (const AppError& cleanup_error) {
        std::string detail = cleanup_error.detail();
        if (session_error) {
            try {
                std::rethrow_exception(session_error);
            } catch (const std::exception& error) {
                detail += "; preceding session failure: " + std::string(error.what());
            } catch (...) {
                detail += "; preceding session failure: unknown exception";
            }
        }
        AppError combined(ExitCode::overlay_protection, cleanup_error.stage(), std::move(detail));
        if (launcher_started) combined.after_launch();
        throw combined;
    }
    if (session_error) {
        try {
            std::rethrow_exception(session_error);
        } catch (const elevation::Cancelled&) {
            throw;
        } catch (const elevation::RelaunchRequired&) {
            throw;
        } catch (AppError& error) {
            if (launcher_started) error.after_launch();
            throw;
        } catch (const std::exception& error) {
            AppError converted(ExitCode::internal, "post-launch session", error.what());
            if (launcher_started) converted.after_launch();
            throw converted;
        } catch (...) {
            AppError converted(ExitCode::internal, "post-launch session", "unknown exception");
            if (launcher_started) converted.after_launch();
            throw converted;
        }
    }
    if (exit_status) {
        log.info("original launcher exit status: " + std::to_string(*exit_status));
    } else {
        log.warn("original launcher exit status was unavailable");
    }
    log.info("permission cleanup successful");
    return exit_status.value_or(0);
}

std::wstring failure_message(const AppError& error) {
    if (error.stage() == "Steam file repair") {
        return L"Steam file repair could not finish.\n\n" + widen(error.detail()) +
               L"\n\nSee steamfix.log in the Destiny 2 directory.";
    }
    if (error.code() == ExitCode::invalid_invocation) return widen(error.detail());
    const wchar_t* launch_status = error.launcher_started()
        ? L"The Destiny 2 launcher was started." : L"Destiny 2 was not started.";
    return L"D2SteamFix failed during " + widen(error.stage()) + L":\n\n" + widen(error.detail()) +
           L"\n\nSee steamfix.log in the Destiny 2 directory.\n\n" + launch_status;
}

bool validate_app_ids(const std::optional<std::wstring>& app_id,
                      const std::optional<std::wstring>& game_id) noexcept {
    const auto invalid = [](const std::optional<std::wstring>& value) {
        return value && *value != L"1085660";
    };
    if (invalid(app_id) || invalid(game_id)) return false;
    return (app_id && *app_id == L"1085660") || (game_id && *game_id == L"1085660");
}

fs::path resolve_launcher(const std::wstring& argument) {
    const fs::path raw(argument);
    if (!paths::name_is(raw, L"destiny2launcher.exe")) {
        throw AppError(ExitCode::validation, "command validation",
                       "the forwarded executable must be destiny2launcher.exe");
    }
    fs::path launcher;
    try {
        launcher = fs::canonical(raw);
    } catch (const std::exception& error) {
        throw AppError(ExitCode::validation, "command validation",
                       "launcher is not an existing regular file: " + std::string(error.what()));
    }
    require_regular_file(launcher, ExitCode::validation, "command validation",
                         "launcher is not an existing regular file");
    if (!paths::name_is(launcher, L"destiny2launcher.exe")) {
        throw AppError(ExitCode::validation, "command validation",
                       "launcher is not a regular destiny2launcher.exe file");
    }
    return launcher;
}

Invocation validate_invocation(const std::vector<std::wstring>& args,
                               const std::optional<std::wstring>& app_id,
                               const std::optional<std::wstring>& game_id) {
    if (args.empty()) {
        throw AppError(ExitCode::invalid_invocation, "invalid invocation", narrow(invalid_message));
    }
    const fs::path launcher = resolve_launcher(args.front());
    const fs::path game_dir = launcher.parent_path();
    if (game_dir.empty()) {
        throw AppError(ExitCode::validation, "command validation", "launcher has no containing directory");
    }
    require_regular_file(game_dir / L"destiny2.exe", ExitCode::validation,
                         "command validation",
                         "destiny2.exe is not a regular file beside the launcher");
    if (!validate_app_ids(app_id, game_id)) {
        throw AppError(ExitCode::validation, "Steam App ID validation",
                       "SteamAppId and SteamGameId must be absent or 1085660, and at least one must equal 1085660");
    }
    return {launcher, game_dir, std::vector<std::wstring>(args.begin() + 1, args.end())};
}

Logger create_log(const fs::path& path) {
    try {
        return Logger(path);
    } catch (const elevation::Cancelled&) {
        throw;
    } catch (const std::exception& error) {
        throw AppError(ExitCode::log_initialization, "log initialization", error.what());
    }
}

} // namespace

int entry() noexcept try {
    std::vector<std::wstring> forwarded;
    std::optional<fs::path> inherited_steam_directory;
    try {
        forwarded = command_line_arguments();
        if (const std::optional<int> helper = elevation::dispatch_helper(forwarded)) return *helper;
        inherited_steam_directory = elevation::accept_elevated_launch(forwarded);
    } catch (const std::exception& error) {
        ui::error(L"D2SteamFix could not read its command line: " + widen(error.what()));
        return static_cast<int>(ExitCode::invalid_invocation);
    }
    fs::path executable;
    try {
        executable = current_executable();
    } catch (const std::exception& error) {
        ui::error(L"D2SteamFix could not determine its executable path: " + widen(error.what()) +
                  L"\n\nDestiny 2 was not started.");
        return static_cast<int>(ExitCode::internal);
    }
    fs::path game_directory;
    try {
        game_directory = forwarded.empty() ? executable.parent_path()
                                            : resolve_launcher(forwarded.front()).parent_path();
    } catch (const AppError& error) {
        ui::error(failure_message(error));
        return static_cast<int>(error.code());
    } catch (const std::exception& error) {
        ui::error(L"D2SteamFix could not determine the Destiny 2 directory for its log and "
                  L"recovery files.\n\n" + widen(error.what()) + L"\n\nDestiny 2 was not started.");
        return static_cast<int>(ExitCode::log_initialization);
    }
    const Logger log = create_log(game_directory / L"steamfix.log");
    log.info("startup; wrapper path: " + display_path(executable) +
              "; game directory: " + display_path(game_directory));
    try {
        const fs::path recovery = acl::recovery_path(game_directory);
        log.info("ACL recovery state path: " + display_path(recovery));
        if (forwarded.empty()) {
            static_cast<void>(repair::run(game_directory, recovery, log));
            log.info("direct Steam repair finished; final exit status: 0");
            return 0;
        }
        const SessionMutex mutex = session_mutex();
        const int code = run(log, forwarded, recovery, inherited_steam_directory);
        log.info("final exit status: " + std::to_string(code));
        return code;
    } catch (const elevation::RelaunchRequired& request) {
        // The protected session and its mutex have both unwound before UAC.
        try {
            log.info("original renderer permissions restored; requesting an administrator D2SteamFix session");
            std::vector<std::wstring> retry = forwarded;
            // Resolve a relative Steam command once, in its original directory.
            retry.front() = request.launcher().native();
            const std::optional<int> code = elevation::relaunch_as_administrator(retry, request.directory(),
                                                                                     request.steam_directory());
            if (code) {
                log.info("administrator D2SteamFix session exit status: " + std::to_string(*code));
            } else {
                log.warn("administrator D2SteamFix session exited; Windows did not provide its exit status");
            }
            return code.value_or(0);
        } catch (const elevation::Cancelled&) {
            log.info("Windows permission request cancelled; original permissions remain restored");
            return 0;
        } catch (const std::exception& error) {
            log.error("administrator session launch or monitoring failed: " + std::string(error.what()));
            ui::error(L"D2SteamFix could not complete its administrator session.\n\n" + widen(error.what()) +
                      L"\n\nOriginal renderer permissions were restored before the administrator session was requested.");
            return static_cast<int>(ExitCode::process_creation);
        }
    } catch (const elevation::Cancelled&) {
        log.info("Windows permission request cancelled; no launcher was started");
        return 0;
    } catch (const AppError& error) {
        log.error(std::string(error.what()) + "; helper exit code=" +
                   std::to_string(static_cast<int>(error.code())));
        ui::error(failure_message(error));
        return static_cast<int>(error.code());
    } catch (const std::exception& error) {
        log.error("unexpected internal failure: " + std::string(error.what()) + "; helper exit code=19");
        ui::error(L"D2SteamFix encountered an unexpected internal failure.\n\nSee steamfix.log in the "
                  L"Destiny 2 directory.\n\nDestiny 2 was not started.");
        return static_cast<int>(ExitCode::internal);
    } catch (...) {
        log.error("unexpected internal failure; helper exit code=19");
        ui::error(L"D2SteamFix encountered an unexpected internal failure.\n\nSee steamfix.log in the "
                  L"Destiny 2 directory.\n\nDestiny 2 was not started.");
        return static_cast<int>(ExitCode::internal);
    }
} catch (const elevation::Cancelled&) {
    return 0;
} catch (const AppError& error) {
    try {
        if (error.code() == ExitCode::log_initialization) {
            ui::error(L"D2SteamFix could not open steamfix.log in the Destiny 2 directory. "
                      L"Ensure the game directory is writable.\n\n" + widen(error.detail()) +
                      L"\n\nDestiny 2 was not started.");
        } else {
            ui::error(failure_message(error));
        }
    } catch (...) {
        ui::error(L"D2SteamFix could not report an initialization failure.");
    }
    return static_cast<int>(error.code());
} catch (...) {
    // Error formatting and startup logging can allocate too. Keep every failure
    // inside the C++ boundary and use an allocation-free last-resort message.
    ui::error(L"D2SteamFix encountered an unexpected failure.\n\n"
              L"If a session was interrupted, keep steamfix.dat for permission recovery.");
    return static_cast<int>(ExitCode::internal);
}

} // namespace steamfix::app
