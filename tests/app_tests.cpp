// Include the implementation to exercise private validation without exposing a
// test-only application API. Link the remaining production modules normally.
#include "../src/app.cpp"

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
void error(std::wstring_view) noexcept {}
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
