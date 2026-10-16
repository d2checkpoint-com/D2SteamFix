// Include the implementation to exercise private validation without exposing a
// test-only application API. Link the remaining production modules normally.
#include "steamfix/elevation.hpp"

#include <aclapi.h>
#include <dpapi.h>
#include <shellapi.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace app_fault {

enum class Kind { process_error, cancelled, relaunch, standard_error, unexpected_error, real_launch };
enum class Retry { status, unknown_status, cancelled, failure };

struct Plan {
    Kind kind = Kind::process_error;
    DWORD error = ERROR_BAD_EXE_FORMAT;
    bool elevated = false;
    std::filesystem::path steam;
    int cleanup_failures = 0;
    std::wstring entry_command{};
    std::filesystem::path launcher{};
    Retry retry = Retry::status;
    bool parse_failure = false;
    bool executable_failure = false;
    bool existing_session = false;
    bool unexpected_startup_failure = false;
};

struct UnexpectedFailure final {};

// Faults affect only the coordinator's call sites in this test translation
// unit. Other production modules continue to use their real implementations.
thread_local const Plan* active = nullptr;
thread_local bool restoring = false;
thread_local int cleanup_failures = 0;
thread_local int retry_calls = 0;
thread_local bool retry_restored = false;
thread_local int error_messages = 0;

class Scope final {
public:
    explicit Scope(const Plan& plan) noexcept
        : previous_(active), previous_restoring_(restoring), previous_failures_(cleanup_failures),
          previous_retry_calls_(retry_calls), previous_retry_restored_(retry_restored),
          previous_messages_(error_messages) {
        active = &plan;
        restoring = false;
        cleanup_failures = plan.cleanup_failures;
        retry_calls = 0;
        retry_restored = false;
        error_messages = 0;
    }
    ~Scope() noexcept {
        active = previous_;
        restoring = previous_restoring_;
        cleanup_failures = previous_failures_;
        retry_calls = previous_retry_calls_;
        retry_restored = previous_retry_restored_;
        error_messages = previous_messages_;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
private:
    const Plan* previous_;
    bool previous_restoring_;
    int previous_failures_;
    int previous_retry_calls_;
    bool previous_retry_restored_;
    int previous_messages_;
};

LPCWSTR command_line() {
    if (active != nullptr && active->unexpected_startup_failure) throw UnexpectedFailure{};
    return active != nullptr && !active->entry_command.empty()
               ? active->entry_command.c_str() : ::GetCommandLineW();
}

LPWSTR* WINAPI parse_command_line(LPCWSTR command, int* count) {
    if (active != nullptr && active->parse_failure) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return ::CommandLineToArgvW(command, count);
}

DWORD WINAPI module_name(HMODULE module, LPWSTR buffer, DWORD size) {
    if (active != nullptr && active->executable_failure) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    return ::GetModuleFileNameW(module, buffer, size);
}

HANDLE WINAPI session_mutex(LPSECURITY_ATTRIBUTES security, BOOL owner, LPCWSTR name) {
    if (active == nullptr || active->entry_command.empty()) return ::CreateMutexW(security, owner, name);
    if (name == nullptr || std::wstring_view(name) != L"Global\\D2SteamFix-1085660") {
        throw std::logic_error("unexpected fixture session mutex");
    }
    const HANDLE handle = ::CreateMutexW(security, owner, nullptr);
    if (handle != nullptr && active->existing_session) SetLastError(ERROR_ALREADY_EXISTS);
    return handle;
}

DWORD WINAPI set_security(const HANDLE handle, const SE_OBJECT_TYPE type, const SECURITY_INFORMATION information,
                          PSID owner, PSID group, PACL dacl, PACL sacl) {
    if (active != nullptr && restoring && cleanup_failures > 0) {
        --cleanup_failures;
        return ERROR_ACCESS_DENIED;
    }
    return ::SetSecurityInfo(handle, type, information, owner, group, dacl, sacl);
}

BOOL WINAPI create_process(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES process_security,
                           LPSECURITY_ATTRIBUTES thread_security, BOOL inherit, DWORD flags,
                           LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup,
                           LPPROCESS_INFORMATION information) {
    if (active != nullptr && active->kind != Kind::real_launch) {
        SetLastError(active->error);
        return FALSE;
    }
    return ::CreateProcessW(application, command, process_security, thread_security, inherit,
                            flags, environment, directory, startup, information);
}

} // namespace app_fault

namespace steamfix::elevation {

std::optional<fs::path> app_test_accept_launch(std::vector<std::wstring>& arguments) {
    if (app_fault::active == nullptr || app_fault::active->entry_command.empty()) return accept_elevated_launch(arguments);
    if (arguments.size() != 2 || fs::path(arguments.front()) != app_fault::active->launcher ||
        arguments.back() != L"--fixture-launcher") {
        throw std::logic_error("entry fixture did not supply its exact owned launcher command");
    }
    return app_fault::active->steam;
}

std::optional<int> app_test_relaunch(const std::vector<std::wstring>& arguments,
                                    const fs::path& directory, const fs::path& steam_directory);

bool app_test_is_elevated() {
    return app_fault::active != nullptr ? app_fault::active->elevated : is_elevated();
}

bool app_test_desktop_launch(const std::filesystem::path& executable, const std::wstring& command,
                             const std::filesystem::path& directory, PROCESS_INFORMATION& information) {
    if (app_fault::active == nullptr) return launch_as_desktop_user(executable, command, directory, information);
    app_fault::restoring = true;
    switch (app_fault::active->kind) {
    case app_fault::Kind::process_error:
    case app_fault::Kind::real_launch:
        return false;
    case app_fault::Kind::cancelled:
        throw Cancelled();
    case app_fault::Kind::relaunch:
        throw RelaunchRequired(app_fault::active->error, directory, executable, app_fault::active->steam);
    case app_fault::Kind::standard_error:
        throw std::runtime_error("injected coordinator launch failure");
    case app_fault::Kind::unexpected_error:
        throw app_fault::UnexpectedFailure{};
    }
    throw std::logic_error("unknown launch-fault kind");
}

} // namespace steamfix::elevation

// Only ACL operations from this included implementation see the finite cleanup
// fault. Targets are always the fixture renderers supplied by check_session.
#define SetSecurityInfo app_fault::set_security
#include "../src/acl.cpp"
#undef SetSecurityInfo

#define CreateProcessW app_fault::create_process
#define is_elevated app_test_is_elevated
#define launch_as_desktop_user app_test_desktop_launch
#define accept_elevated_launch app_test_accept_launch
#define relaunch_as_administrator app_test_relaunch
#define GetCommandLineW app_fault::command_line
#define CommandLineToArgvW app_fault::parse_command_line
#define GetModuleFileNameW app_fault::module_name
#define CreateMutexW app_fault::session_mutex
#include "../src/app.cpp"
#undef CreateMutexW
#undef GetModuleFileNameW
#undef CommandLineToArgvW
#undef GetCommandLineW
#undef relaunch_as_administrator
#undef accept_elevated_launch
#undef CreateProcessW
#undef is_elevated
#undef launch_as_desktop_user

namespace steamfix::elevation {

std::optional<int> app_test_relaunch(const std::vector<std::wstring>& arguments,
                                    const fs::path& directory, const fs::path& steam_directory) {
    if (app_fault::active == nullptr || app_fault::active->entry_command.empty()) {
        return relaunch_as_administrator(arguments, directory, steam_directory);
    }
    ++app_fault::retry_calls;
    if (arguments.size() != 2 || !paths::equal_windows(fs::path(arguments.front()), app_fault::active->launcher) ||
        arguments.back() != L"--fixture-launcher" ||
        !paths::equal_windows(directory, app_fault::active->launcher.parent_path()) ||
        !paths::equal_windows(steam_directory, app_fault::active->steam) ||
        fs::exists(directory / L"steamfix.dat")) {
        throw std::logic_error("administrator retry preceded restoration or escaped its fixture");
    }
    const Logger log(directory / L"steamfix.log");
    if (repair::needed(steam_directory, log)) throw std::logic_error("administrator retry retained execute denial");
    app_fault::retry_restored = true;
    switch (app_fault::active->retry) {
    case app_fault::Retry::status: return 23;
    case app_fault::Retry::unknown_status: return std::nullopt;
    case app_fault::Retry::cancelled: throw Cancelled();
    case app_fault::Retry::failure: throw std::runtime_error("controlled administrator launch failure");
    }
    throw std::logic_error("unknown administrator retry fixture");
}

} // namespace steamfix::elevation

#include <objbase.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

void require(const bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <std::invocable Function>
void expect_error(const steamfix::ExitCode code, Function&& action) {
    try {
        action();
    } catch (const steamfix::AppError& error) {
        require(error.code() == code, "unexpected application error code");
        return;
    }
    throw std::runtime_error("expected an application error");
}

class TestDirectory final {
public:
    TestDirectory() {
        GUID identifier{};
        require(SUCCEEDED(CoCreateGuid(&identifier)), "create fixture identifier");
        std::array<wchar_t, 40> text{};
        require(StringFromGUID2(identifier, text.data(), static_cast<int>(text.size())) != 0,
                "format fixture identifier");
        path_ = fs::temp_directory_path() / (L"D2SteamFix app tests " + std::wstring(text.data()));
        require(fs::create_directory(path_), "create fixture directory");
    }
    ~TestDirectory() noexcept {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;
    TestDirectory(TestDirectory&&) = delete;
    TestDirectory& operator=(TestDirectory&&) = delete;
    const fs::path& path() const noexcept { return path_; }
private:
    fs::path path_;
};

class Environment final {
public:
    Environment(const wchar_t* name, const wchar_t* value)
        : name_(name), previous_(steamfix::app::environment(name)) {
        require(SetEnvironmentVariableW(name_.c_str(), value) != FALSE, "set fixture environment");
    }
    ~Environment() noexcept {
        SetEnvironmentVariableW(name_.c_str(), previous_ ? previous_->c_str() : nullptr);
    }
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
    Environment(Environment&&) = delete;
    Environment& operator=(Environment&&) = delete;
private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

void write_file(const fs::path& path, const std::string_view contents = "fixture") {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    require(static_cast<bool>(stream), "write fixture file");
}

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "read fixture file");
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void check_errors() {
    using steamfix::AppError;
    using steamfix::ExitCode;
    static_assert(std::is_nothrow_copy_constructible_v<AppError>);
    static_assert(std::is_nothrow_copy_assignable_v<AppError>);
    static_assert(std::is_nothrow_move_constructible_v<AppError>);
    static_assert(std::is_nothrow_move_assignable_v<AppError>);
    AppError error(ExitCode::validation, "validation", "detail");
    require(std::string_view{error.what()} == "validation: detail", "exception message");
    require(!error.launcher_started(), "initial launch state");
    error.after_launch();
    AppError copied(error);
    AppError moved(std::move(error));
    copied = moved;
    copied = std::move(moved);
    // AppError explicitly implements move as a nonthrowing copy, retaining both
    // accessor invariants. This assertion intentionally checks that contract.
#ifdef _MSC_VER
#pragma warning(suppress : 26800)
#endif
    require(error.stage() == "validation" && moved.detail() == "detail" && copied.launcher_started(),
            "exception copies and moves preserve context");
    const auto captured = std::make_exception_ptr(copied);
    try {
        std::rethrow_exception(captured);
    } catch (const AppError& caught) {
        require(caught.detail() == "detail" && caught.launcher_started(), "exception_ptr context");
    }
    require(steamfix::app::failure_message(copied).find(L"launcher was started") != std::wstring::npos,
            "post-launch message");
    const AppError before(ExitCode::validation, "validation", "detail");
    require(steamfix::app::failure_message(before).find(L"was not started") != std::wstring::npos,
            "pre-launch message");
    const AppError invocation(ExitCode::invalid_invocation, "invocation", "exact detail");
    require(steamfix::app::failure_message(invocation) == L"exact detail", "invocation message");
    const AppError repair(ExitCode::internal, "Steam file repair", "repair detail");
    require(steamfix::app::failure_message(repair).find(L"repair detail") != std::wstring::npos,
            "repair message");
}

void check_validation(const fs::path& directory) {
    using namespace steamfix;
    const std::array<std::optional<std::wstring>, 3> ids{std::nullopt, L"1085660", L"7"};
    for (const auto& app_value : ids) {
        for (const auto& game : ids) {
            const bool expected = (app_value != ids[2] && game != ids[2]) &&
                                  (app_value == ids[1] || game == ids[1]);
            require(app::validate_app_ids(app_value, game) == expected, "Steam App ID combinations");
        }
    }
    require(app::optional_id(std::nullopt) == L"None" &&
            app::optional_id(std::wstring{L"1085660"}) == L"Some(\"1085660\")", "optional ID display");
    expect_error(ExitCode::invalid_invocation, [] { app::validate_invocation({}, {}, {}); });
    expect_error(ExitCode::validation, [] { app::resolve_launcher(L"other.exe"); });
    expect_error(ExitCode::validation, [&] { app::resolve_launcher((directory / L"destiny2launcher.exe").native()); });
    const fs::path launcher = directory / L"destiny2launcher.exe";
    fs::create_directory(launcher);
    expect_error(ExitCode::validation, [&] { app::resolve_launcher(launcher.native()); });
    fs::remove(launcher);
    write_file(launcher);
    expect_error(ExitCode::validation, [&] { app::validate_invocation({launcher.native()}, ids[1], {}); });
    write_file(directory / L"destiny2.exe");
    expect_error(ExitCode::validation, [&] { app::validate_invocation({launcher.native()}, {}, {}); });
    const auto invocation = app::validate_invocation({launcher.native(), L"two words", L""}, ids[1], {});
    require(paths::equal_windows(invocation.game_dir, directory) && invocation.args.size() == 2 &&
            invocation.args.front() == L"two words" && invocation.args.back().empty(), "forwarded arguments");
    const fs::path old_directory = fs::current_path();
    fs::current_path(directory);
    try {
        const auto relative = app::validate_invocation({L"destiny2launcher.exe"}, {}, ids[1]);
        require(relative.launcher.is_absolute() && paths::equal_windows(relative.game_dir, directory),
                "relative launcher resolves before working directory changes");
    } catch (...) {
        fs::current_path(old_directory);
        throw;
    }
    fs::current_path(old_directory);
    require(app::existing_processes({}) == "verified Destiny processes already running: []", "empty process display");
    require(app::existing_processes({{1, L"destiny2.exe"}, {2, L"destiny2launcher.exe"}}).find(", (2,") !=
            std::string::npos, "multiple process display");
    app::check_overlay();
    const auto mutex = app::session_mutex();
    expect_error(ExitCode::existing_session, [] { static_cast<void>(app::session_mutex()); });
}

void check_environment() {
    constexpr auto name = L"D2STEAMFIX_TEST_VALUE";
    const Environment guard(name, nullptr);
    require(!steamfix::app::environment(name), "missing environment value");
    require(SetEnvironmentVariableW(name, L"") != FALSE, "set empty environment");
    require(steamfix::app::environment(name) == std::wstring{}, "empty environment value");
    require(SetEnvironmentVariableW(name, L"two words 修復") != FALSE, "set Unicode environment");
    require(steamfix::app::environment(name) == std::wstring{L"two words 修復"}, "Unicode environment value");
    require(steamfix::app::current_executable().is_absolute(), "current executable path");
    require(steamfix::app::command_line_arguments() == std::vector<std::wstring>{L"--entry-check"},
            "command-line argument ownership");
}

void check_logging(const fs::path& directory) {
    const fs::path path = directory / L"steamfix.log";
    {
        const steamfix::Logger log(path);
        log.info("first\nsecond\n");
        log.warn("");
        log.error("failure");
        std::array<std::jthread, 4> writers;
        for (auto& writer : writers) {
            writer = std::jthread([&] { for (int i = 0; i < 10; ++i) log.info("parallel complete line"); });
        }
    }
    const std::string text = read_file(path);
    require(text.find("[INFO]") != std::string::npos && text.find("[WARN]") != std::string::npos &&
            text.find("[ERROR]") != std::string::npos && text.find("first\n") != std::string::npos,
            "log levels and multiline formatting");
    require(std::count(text.begin(), text.end(), '\n') == 45, "concurrent log messages stay complete");
    const fs::path linked = directory / L"hardlink.log";
    require(CreateHardLinkW(linked.c_str(), path.c_str(), nullptr) != FALSE, "create log hardlink fixture");
    try {
        const steamfix::Logger unsafe(path);
        throw std::logic_error("expected linked log rejection");
    } catch (const std::system_error& error) {
        require(error.code().category() == std::system_category() &&
                error.code().value() == ERROR_INVALID_DATA, "hardlinked log rejected before rotation");
    }
    expect_error(steamfix::ExitCode::log_initialization,
                 [&] { const auto rejected = steamfix::app::create_log(path); });
    fs::remove(linked);
}

int fixture_launcher() {
    const fs::path game = steamfix::app::current_executable().parent_path() / L"destiny2.exe";
    std::wstring command;
    steamfix::command_line::append_quoted(command, game.native());
    steamfix::command_line::append_quoted(command, L"--fixture-game");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    require(CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           game.parent_path().c_str(), &startup, &information) != FALSE, "launch fixture game");
    const steamfix::UniqueHandle process(information.hProcess);
    const steamfix::UniqueHandle thread(information.hThread);
    require(WaitForSingleObject(process.get(), 10000) == WAIT_OBJECT_0, "fixture game exits");
    return 7;
}

int authenticated_entry(const fs::path& steam, const fs::path& launcher) {
    // An already elevated test can exercise the authenticated coordinator
    // handoff with CreateProcess, avoiding any runas/UAC prompt or registry edit.
    const fs::path self = steamfix::app::current_executable();
    std::wstring command;
    for (const auto& argument : std::array{self.native(), std::wstring{L"--steamfix-elevated-launch"},
                                         std::to_wstring(GetCurrentProcessId()), steam.native(),
                                         launcher.native(), std::wstring{L"--fixture-launcher"}}) {
        steamfix::command_line::append_quoted(command, argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    require(CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           steam.parent_path().c_str(), &startup, &information) != FALSE,
            "create authenticated entry fixture");
    const steamfix::UniqueHandle child(information.hProcess);
    const steamfix::UniqueHandle thread(information.hThread);
    if (WaitForSingleObject(child.get(), 45000) != WAIT_OBJECT_0) {
        // This is the child just created above, never a discovered process.
        TerminateProcess(child.get(), 99);
        WaitForSingleObject(child.get(), 5000);
        throw std::runtime_error("authenticated entry fixture did not finish");
    }
    DWORD status{};
    require(GetExitCodeProcess(child.get(), &status) != FALSE, "read entry fixture status");
    return steamfix::windows_exit_status(status);
}

void check_session(const fs::path& directory) {
    using namespace steamfix;
    const Environment app_id(L"SteamAppId", L"1085660");
    const Environment game_id(L"SteamGameId", nullptr);
    const fs::path game = directory / L"game with spaces";
    const fs::path steam = directory / L"steam with spaces";
    fs::create_directory(game);
    fs::create_directory(steam);
    const fs::path self = app::current_executable();
    const fs::path launcher = game / L"destiny2launcher.exe";
    fs::copy_file(self, launcher);
    fs::copy_file(self, game / L"destiny2.exe");
    write_file(steam / L"steam.exe");
    const fs::path renderer = steam / L"GameOverlayRenderer64.dll";
    write_file(renderer, "immutable renderer contents");
    const fs::path renderer32 = steam / L"GameOverlayRenderer.dll";
    write_file(renderer32, "immutable 32-bit contents");
    const Logger log(game / L"steamfix.log");
    const fs::path recovery = acl::recovery_path(game);
    const std::vector<std::wstring> arguments{launcher.native(), L"--fixture-launcher"};
    require(app::run(log, arguments, recovery, steam) == 7, "launcher status survives protected session");
    if (elevation::is_elevated()) {
        const int status = authenticated_entry(steam, launcher);
        if (status != 7) {
            throw std::runtime_error("authenticated application entry failed with status " +
                                     std::to_string(status) + ": " + read_file(game / L"steamfix.log"));
        }
        const fs::path linked_log = game / L"linked.log";
        require(CreateHardLinkW(linked_log.c_str(), (game / L"steamfix.log").c_str(), nullptr) != FALSE,
                "create entry log-failure fixture");
        require(authenticated_entry(steam, launcher) == static_cast<int>(ExitCode::log_initialization),
                "entry retains log initialization error code");
        fs::remove(linked_log);
    } else {
        std::cout << "SKIP: authenticated elevated entry requires an already elevated test token.\n";
    }
    require(!fs::exists(recovery) && read_file(renderer) == "immutable renderer contents" &&
            read_file(renderer32) == "immutable 32-bit contents", "session restores and cleans journal");
    const auto check_restored = [&] {
        require(!fs::exists(recovery) && !repair::needed(steam, log) &&
                read_file(renderer) == "immutable renderer contents", "launch faults preserve original permissions");
    };
    for (const auto kind : {app_fault::Kind::cancelled, app_fault::Kind::relaunch,
                            app_fault::Kind::standard_error, app_fault::Kind::unexpected_error}) {
        const app_fault::Plan plan{kind, ERROR_ACCESS_DENIED, false, steam};
        const app_fault::Scope faults(plan);
        bool caught = false;
        try {
            app::run(log, arguments, recovery, steam);
        } catch (const elevation::Cancelled&) {
            caught = kind == app_fault::Kind::cancelled;
        } catch (const elevation::RelaunchRequired& request) {
            caught = kind == app_fault::Kind::relaunch && paths::equal_windows(request.launcher(), launcher) &&
                     paths::equal_windows(request.steam_directory(), steam);
        } catch (const AppError& error) {
            caught = (kind == app_fault::Kind::standard_error || kind == app_fault::Kind::unexpected_error) &&
                     error.code() == ExitCode::internal && !error.launcher_started();
        }
        require(caught, "coordinator preserves injected failure identity after restoration");
        check_restored();
    }
    for (const DWORD error : {ERROR_ACCESS_DENIED, ERROR_PRIVILEGE_NOT_HELD, ERROR_ELEVATION_REQUIRED}) {
        for (const bool elevated : {false, true}) {
            const app_fault::Plan plan{app_fault::Kind::process_error, error, elevated, steam};
            const app_fault::Scope faults(plan);
            bool caught = false;
            try {
                app::run(log, arguments, recovery, steam);
            } catch (const elevation::RelaunchRequired& request) {
                caught = !elevated && paths::equal_windows(request.directory(), game);
            } catch (const AppError& failure) {
                caught = elevated && failure.code() == ExitCode::process_creation && !failure.launcher_started();
            }
            require(caught, "permission failure selects administrator retry only when appropriate");
            check_restored();
        }
    }
    for (const auto kind : {app_fault::Kind::standard_error, app_fault::Kind::unexpected_error,
                            app_fault::Kind::real_launch}) {
        const app_fault::Plan plan{kind, ERROR_ACCESS_DENIED, false, steam, 1};
        const app_fault::Scope faults(plan);
        bool caught = false;
        try {
            app::run(log, arguments, recovery, steam);
        } catch (const AppError& error) {
            caught = error.code() == ExitCode::overlay_protection &&
                     error.launcher_started() == (kind == app_fault::Kind::real_launch);
            const bool preceding = error.detail().find("preceding session failure") != std::string::npos;
            require(preceding == (kind != app_fault::Kind::real_launch),
                    "cleanup reporting retains preceding failures without inventing one");
        }
        require(caught && app_fault::cleanup_failures == 0, "cleanup failure keeps correct session context");
        // Explicit cleanup reports the failure; the resource destructor retries
        // the consumed transient fault and restores the original permissions.
        check_restored();
    }
    std::wstring entry_command;
    command_line::append_quoted(entry_command, self.native());
    for (const auto& argument : arguments) command_line::append_quoted(entry_command, argument);
    for (const auto retry : {app_fault::Retry::status, app_fault::Retry::unknown_status,
                             app_fault::Retry::cancelled, app_fault::Retry::failure}) {
        const app_fault::Plan plan{app_fault::Kind::relaunch, ERROR_ACCESS_DENIED, false, steam,
                                   0, entry_command, launcher, retry};
        const app_fault::Scope faults(plan);
        const int expected = retry == app_fault::Retry::status ? 23 :
                             retry == app_fault::Retry::failure ? static_cast<int>(ExitCode::process_creation) : 0;
        const int status = app::entry();
        if (status != expected || app_fault::retry_calls != 1 || !app_fault::retry_restored) {
            throw std::runtime_error("entry administrator retry: status=" + std::to_string(status) +
                                     ", expected=" + std::to_string(expected) + ", calls=" +
                                     std::to_string(app_fault::retry_calls) + ", restored=" +
                                     std::to_string(app_fault::retry_restored) + ": " +
                                     read_file(game / L"steamfix.log"));
        }
        require(app_fault::error_messages == (retry == app_fault::Retry::failure ? 1 : 0),
                "entry reports administrator retry failures without treating cancellation as an error");
        check_restored();
    }
    for (const auto kind : {app_fault::Kind::process_error, app_fault::Kind::cancelled,
                            app_fault::Kind::standard_error, app_fault::Kind::unexpected_error}) {
        const app_fault::Plan plan{kind, ERROR_BAD_EXE_FORMAT, false, steam, 0, entry_command, launcher};
        const app_fault::Scope faults(plan);
        const int expected = kind == app_fault::Kind::cancelled ? 0 :
                             static_cast<int>(kind == app_fault::Kind::process_error ?
                                              ExitCode::process_creation : ExitCode::internal);
        require(app::entry() == expected && app_fault::retry_calls == 0,
                "entry translates coordinator failures without requesting administrator launch");
        require(app_fault::error_messages == (kind == app_fault::Kind::cancelled ? 0 : 1),
                "entry emits exactly one diagnostic for a noncancelled failure");
        check_restored();
    }
    for (int failure = 0; failure < 4; ++failure) {
        const app_fault::Plan plan{app_fault::Kind::process_error, ERROR_BAD_EXE_FORMAT, false, steam,
                                   0, entry_command, launcher, app_fault::Retry::status,
                                   failure == 0, failure == 1, failure == 2, failure == 3};
        const app_fault::Scope faults(plan);
        const int expected = static_cast<int>(failure == 0 ? ExitCode::invalid_invocation :
                                              failure == 2 ? ExitCode::existing_session : ExitCode::internal);
        require(app::entry() == expected && app_fault::retry_calls == 0 && app_fault::error_messages == 1,
                "entry rejects startup failures inside its nonthrowing boundary");
        check_restored();
    }
    // A valid path with invalid executable bytes forces CreateProcess failure
    // after the recovery journal and execute denial have been established.
    write_file(launcher, "invalid executable");
    expect_error(ExitCode::process_creation, [&] { app::run(log, arguments, recovery, steam); });
    require(!fs::exists(recovery) && !repair::needed(steam, log), "launch failure restores execute access");
    fs::remove(renderer32);
    expect_error(ExitCode::process_creation, [&] { app::run(log, arguments, recovery, steam); });
    require(!fs::exists(recovery), "optional 32-bit absence preserves cleanup");
    write_file(recovery, "invalid journal");
    expect_error(ExitCode::overlay_protection, [&] { app::run(log, arguments, recovery, steam); });
    require(read_file(recovery) == "invalid journal", "invalid journal preserved");
    fs::remove(recovery);
    fs::remove(renderer);
    expect_error(ExitCode::steam_discovery, [&] { app::run(log, arguments, recovery, steam); });
}

} // namespace

// Unit tests capture dialogs to keep CI noninteractive. ui.cpp is still included
// separately in the coverage inventory, so its untested lines remain visible.
namespace steamfix::ui {
void info(std::wstring_view) noexcept {}
void error(std::wstring_view) noexcept {
    if (app_fault::active != nullptr) ++app_fault::error_messages;
}
}

int main() {
    try {
        const auto arguments = steamfix::app::command_line_arguments();
        if (!arguments.empty() && arguments.front() == L"--steamfix-elevated-launch") {
            return steamfix::app::entry();
        }
        if (arguments == std::vector<std::wstring>{L"--fixture-game"}) {
            Sleep(2000);
            return 0;
        }
        if (arguments == std::vector<std::wstring>{L"--fixture-launcher"}) return fixture_launcher();
        require(arguments == std::vector<std::wstring>{L"--entry-check"}, "test invocation");
        const TestDirectory fixture;
        check_errors();
        check_environment();
        check_validation(fixture.path());
        check_logging(fixture.path());
        check_session(fixture.path());
        require(steamfix::app::entry() == static_cast<int>(steamfix::ExitCode::validation),
                "entry rejects unrelated forwarded executable without escape");
    } catch (const std::exception& error) {
        std::cerr << "Application tests failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
