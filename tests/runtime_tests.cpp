// Compile the production implementations into this test executable so their
// internal safety decisions are exercised without adding public test hooks.
#include "../src/paths.cpp"
#include "../src/process.cpp"
#include "../src/repair.cpp"
#include "../src/steam.cpp"
#include "../src/overlay.cpp"

#include <sddl.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using steamfix::UniqueHandle;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception = std::exception, std::invocable Operation>
    requires std::derived_from<Exception, std::exception>
void expect_failure(Operation operation, const char* message) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void check_status(const LSTATUS status) {
    if (status != ERROR_SUCCESS) {
        throw std::system_error(steamfix::windows_exit_status(static_cast<DWORD>(status)),
                                std::system_category());
    }
}

template<std::invocable Operation>
void expect_windows_failure(Operation operation, const DWORD expected, const char* message) {
    try {
        operation();
    } catch (const std::system_error& error) {
        require(static_cast<DWORD>(error.code().value()) == expected, message);
        return;
    }
    throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        for (int attempt = 0; attempt < 10; ++attempt) {
            path_ = fs::temp_directory_path() /
                (L"D2SteamFix-runtime-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (fs::create_directory(path_)) return;
        }
        throw std::runtime_error("could not create an isolated runtime test directory");
    }
    ~TemporaryDirectory() noexcept {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
private:
    fs::path path_;
};

void write_file(const fs::path& path) {
    std::ofstream file(path, std::ios::binary);
    file << "test-owned file\n";
    file.close();
    require(!file.fail(), "could not write a runtime test fixture");
}

class OwnedChild {
public:
    explicit OwnedChild(const fs::path& executable) {
        std::wstring line;
        steamfix::command_line::append_quoted(line, executable.native());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                            executable.parent_path().c_str(), &startup, &information)) {
            throw std::system_error(steamfix::windows_exit_status(GetLastError()),
                                    std::system_category());
        }
        process_ = UniqueHandle(information.hProcess);
        thread_ = UniqueHandle(information.hThread);
        pid_ = information.dwProcessId;
    }
    ~OwnedChild() noexcept {
        if (process_.get() != nullptr) {
            // This handle comes only from this fixture's CreateProcess call.
            TerminateProcess(process_.get(), 0);
            WaitForSingleObject(process_.get(), 5000);
        }
    }
    OwnedChild(const OwnedChild&) = delete;
    OwnedChild& operator=(const OwnedChild&) = delete;
    OwnedChild(OwnedChild&&) = delete;
    OwnedChild& operator=(OwnedChild&&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return process_.get(); }
    [[nodiscard]] DWORD pid() const noexcept { return pid_; }
    void finish(const DWORD status) {
        require(TerminateProcess(process_.get(), status) != FALSE,
                "could not finish a test-owned child");
        require(WaitForSingleObject(process_.get(), 5000) == WAIT_OBJECT_0,
                "test-owned child did not exit");
    }
private:
    UniqueHandle process_;
    UniqueHandle thread_;
    DWORD pid_ = 0;
};

void check_release_state() {
    using steamfix::process::ReleaseState;
    const auto start = ReleaseState::Clock::time_point{};
    const auto at = [start](const int seconds) {
        return start + std::chrono::seconds(seconds);
    };

    ReleaseState unseen;
    require(!unseen.observe(false, false, true, at(0)), "initial absence must start grace");
    require(!unseen.observe(false, false, true, at(29)), "unseen game must retain 30-second grace");
    require(!unseen.observe(false, false, true, at(30)), "grace expiry needs a debounce poll");
    require(unseen.observe(false, false, true, at(31)), "two absences after grace should release");

    ReleaseState played;
    require(!played.observe(true, false, true, at(0)), "live launcher must retain protection");
    require(!played.observe(false, true, true, at(1)), "live game must retain protection");
    require(!played.observe(false, false, true, at(2)), "first absence must retain protection");
    require(!played.observe(false, false, false, at(3)), "unreliable snapshot must retain protection");
    require(!played.observe(false, false, true, at(4)), "unreliable snapshot must reset debounce");
    require(played.observe(false, false, true, at(5)), "observed game needs two reliable absences");

    ReleaseState restarted;
    require(!restarted.observe(false, false, true, at(0)), "absence should start grace");
    require(!restarted.observe(true, false, true, at(30)), "restarted launcher should reset grace");
    require(!restarted.observe(false, false, true, at(31)), "new absence should start fresh grace");
    require(!restarted.observe(false, false, true, at(60)), "fresh grace must last 30 seconds");
    require(!restarted.observe(false, false, true, at(61)), "fresh grace needs first negative poll");
    require(restarted.observe(false, false, true, at(62)), "fresh grace should release after debounce");

    ReleaseState uncertain_game;
    require(!uncertain_game.observe(false, true, false, at(0)), "uncertain live game retains protection");
    require(!uncertain_game.observe(false, false, true, at(1)), "observed game remains remembered");
    require(uncertain_game.observe(false, false, true, at(2)), "observed game skips launcher-only grace");

    ReleaseState interrupted_grace;
    require(!interrupted_grace.observe(false, false, true, at(0)), "grace should begin");
    require(!interrupted_grace.observe(false, false, false, at(29)), "uncertainty must reset grace");
    require(!interrupted_grace.observe(false, false, true, at(30)), "reliable observation restarts grace");
    require(!interrupted_grace.observe(false, false, true, at(59)), "reset grace must remain active");
    require(!interrupted_grace.observe(false, false, true, at(60)), "reset grace first negative poll");
    require(interrupted_grace.observe(false, false, true, at(61)), "reset grace second negative poll");

    int failures = 0;
    steamfix::process::increment_saturated(failures);
    require(failures == 1, "poll counter must increment");
    failures = std::numeric_limits<int>::max() - 1;
    steamfix::process::increment_saturated(failures);
    steamfix::process::increment_saturated(failures);
    require(failures == std::numeric_limits<int>::max(), "poll counter must saturate without overflow");
}

void check_paths_and_installations(const fs::path& root) {
    const fs::path steam = root / L"Steam";
    fs::create_directories(steam / L"bin" / L"helpers");
    write_file(steam / L"steam.exe");
    require(steamfix::paths::name_is(steam / L"STEAM.EXE", L"steam.exe"), "names must ignore case");
    require(!steamfix::paths::name_is(steam / L"steam.exe.old", L"steam.exe"), "names must match whole basename");
    require(!steamfix::paths::name_is(steam / L"steam.exe", L""), "different name lengths must differ");
    require(steamfix::paths::equal_windows(steam, steam / L"."), "equivalent directories must compare equal");
    require(steamfix::paths::equal_windows(root / L"missing", root / L"MISSING"), "missing paths compare without case");
    require(!steamfix::paths::equal_windows(root / L"first", root / L"second"), "different paths must differ");
    require(steamfix::steam::verify(steam) == fs::canonical(steam), "Steam directory must canonicalize");
    require(steamfix::steam::verify(steam / L"steam.exe") == fs::canonical(steam), "Steam executable must resolve directory");
    expect_failure<fs::filesystem_error>([&] { static_cast<void>(steamfix::steam::verify(root / L"absent")); },
                                         "missing Steam installation was accepted");
    require(steamfix::repair::validate_steam(steam / L"steam.exe") == fs::canonical(steam),
            "repair must accept a regular Steam executable");
    expect_failure<std::runtime_error>([&] { static_cast<void>(steamfix::repair::validate_steam(steam / L"other.exe")); },
                                       "repair accepted a different executable name");
    expect_failure<std::runtime_error>([&] { static_cast<void>(steamfix::repair::validate_steam(root / L"steam.exe")); },
                                       "repair accepted a missing Steam executable");

    require(steamfix::repair::destiny_name(L"DESTINY2.EXE"), "game name comparison must ignore case");
    require(steamfix::repair::destiny_name(L"destiny2launcher.exe"), "launcher name must be recognized");
    require(!steamfix::repair::destiny_name(L"other.exe"), "unrelated game name accepted");
    for (const auto* name : {L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe", L"GameOverlayUI.exe",
                             L"steamerrorreporter.exe", L"steamerrorreporter64.exe", L"steam_monitor.exe"}) {
        require(steamfix::repair::steam_name(name), "documented Steam process name was rejected");
    }
    require(!steamfix::repair::steam_name(L"steam.exe.old"), "unrelated Steam process name accepted");
    require(steamfix::repair::inside_steam(steam / L"steam.exe", steam), "Steam root executable must be inside installation");
    require(steamfix::repair::inside_steam(steam / L"bin" / L"helpers" / L"helper.exe", steam), "Steam helper must be inside installation");
    require(!steamfix::repair::inside_steam(steam / L"steamapps" / L"game.exe", steam), "steamapps executable must be excluded");
    require(!steamfix::repair::inside_steam(root / L"other.exe", steam), "outside executable must be excluded");
    fs::path deep = steam;
    for (int depth = 0; depth < 9; ++depth) deep /= L"nested";
    require(!steamfix::repair::inside_steam(deep / L"helper.exe", steam), "containment must respect ancestor limit");
    require(!steamfix::repair::shared_steam_service(steam / L"other.exe"), "unrelated executable cannot be shared service");
    require(!steamfix::repair::shared_steam_service(steam / L"steamservice.exe"), "nonshared service location accepted");
    for (const int folder : {CSIDL_PROGRAM_FILES_COMMONX86, CSIDL_PROGRAM_FILES_COMMON}) {
        std::array<wchar_t, MAX_PATH> common{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, folder, nullptr, SHGFP_TYPE_CURRENT, common.data()))) {
            const fs::path service = fs::path(common.data()) / L"Steam" / L"SteamService.exe";
            require(steamfix::repair::shared_steam_service(service), "shared Steam service location was rejected");
            require(steamfix::repair::steam_process(service, steam), "shared Steam service process was rejected");
        }
    }
    require(steamfix::repair::steam_process(steam / L"steam.exe", steam), "selected Steam installation was rejected");
    require(steamfix::repair::steam_process(steam / L"bin" / L"helper.exe", root / L"different"),
            "another validated Steam installation must be recognized");
    require(!steamfix::repair::steam_process(steam / L"steamapps" / L"game.exe", root / L"different"),
            "game install cannot count as Steam installation");
    require(!steamfix::repair::steam_process(root / L"elsewhere" / L"helper.exe", steam),
            "unvalidated executable location accepted");
    require(!steamfix::repair::steam_process(deep / L"helper.exe", root / L"different"),
            "Steam installation search exceeded its ancestor limit");
}

struct CloseRegistryKey {
    void operator()(HKEY key) const noexcept { RegCloseKey(key); }
};
using TestRegistryKey = std::unique_ptr<std::remove_pointer_t<HKEY>, CloseRegistryKey>;

TestRegistryKey create_key(const HKEY parent, const wchar_t* name) {
    HKEY raw = nullptr;
    check_status(RegCreateKeyExW(parent, name, 0, nullptr, REG_OPTION_VOLATILE,
                                 KEY_ALL_ACCESS, nullptr, &raw, nullptr));
    return TestRegistryKey(raw);
}

class RegistryTree {
public:
    RegistryTree()
        : path_(L"Software\\D2SteamFixTests\\" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64())),
          key_(create_key(HKEY_CURRENT_USER, path_.c_str())) {}
    ~RegistryTree() noexcept {
        key_.reset();
        RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
    }
    RegistryTree(const RegistryTree&) = delete;
    RegistryTree& operator=(const RegistryTree&) = delete;
    RegistryTree(RegistryTree&&) = delete;
    RegistryTree& operator=(RegistryTree&&) = delete;
    [[nodiscard]] HKEY get() const noexcept { return key_.get(); }
private:
    std::wstring path_;
    TestRegistryKey key_;
};

class RegistryOverride {
public:
    explicit RegistryOverride(const HKEY isolated_root) {
        check_status(RegOverridePredefKey(HKEY_CURRENT_USER, isolated_root));
    }
    ~RegistryOverride() noexcept { RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); }
    RegistryOverride(const RegistryOverride&) = delete;
    RegistryOverride& operator=(const RegistryOverride&) = delete;
    RegistryOverride(RegistryOverride&&) = delete;
    RegistryOverride& operator=(RegistryOverride&&) = delete;
};

class EnvironmentVariable {
public:
    EnvironmentVariable(std::wstring name, const std::wstring& value)
        : name_(std::move(name)) {
        const DWORD needed = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
        if (needed != 0) {
            std::wstring old(needed, L'\0');
            const DWORD written = GetEnvironmentVariableW(name_.c_str(), old.data(), needed);
            require(written < needed, "test environment variable changed during inspection");
            old.resize(written);
            previous_ = std::move(old);
        }
        require(SetEnvironmentVariableW(name_.c_str(), value.c_str()) != FALSE,
                "could not set a process-local test environment variable");
    }
    ~EnvironmentVariable() noexcept {
        SetEnvironmentVariableW(name_.c_str(), previous_ ? previous_->c_str() : nullptr);
    }
    EnvironmentVariable(const EnvironmentVariable&) = delete;
    EnvironmentVariable& operator=(const EnvironmentVariable&) = delete;
    EnvironmentVariable(EnvironmentVariable&&) = delete;
    EnvironmentVariable& operator=(EnvironmentVariable&&) = delete;
private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

void check_registry(const fs::path& root) {
    RegistryTree tree;
    const RegistryOverride isolated(tree.get());
    expect_failure<std::system_error>([] { static_cast<void>(steamfix::steam::registry_string(L"SteamExe")); },
                                      "missing Steam registry key was accepted");
    const TestRegistryKey key = create_key(tree.get(), L"Software\\Valve\\Steam");
    const auto put = [&](const wchar_t* name, const std::wstring_view value, const DWORD type = REG_SZ) {
        require(value.size() < std::numeric_limits<DWORD>::max() / sizeof(wchar_t), "test registry value too large");
        check_status(RegSetKeyValueW(key.get(), nullptr, name, type, value.data(),
                                     static_cast<DWORD>(value.size() * sizeof(wchar_t))));
    };
    expect_failure<std::system_error>([] { static_cast<void>(steamfix::steam::registry_string(L"SteamExe")); },
                                      "missing Steam registry value was accepted");
    put(L"SteamExe", L"C:\\Steam\\steam.exe");
    require(steamfix::steam::registry_string(L"SteamExe") == L"C:\\Steam\\steam.exe",
            "nonterminated registry string must be read safely");
    constexpr std::wstring_view padded{L"C:\\Steam\0\0", std::size(L"C:\\Steam\0\0") - 1U};
    put(L"SteamPath", padded);
    require(steamfix::steam::registry_string(L"SteamPath") == L"C:\\Steam", "trailing registry NULs must be trimmed");
    constexpr std::wstring_view embedded{L"C:\\Steam\0hidden", std::size(L"C:\\Steam\0hidden") - 1U};
    put(L"SteamExe", embedded);
    expect_failure<std::runtime_error>([] { static_cast<void>(steamfix::steam::registry_string(L"SteamExe")); },
                                       "embedded registry NUL was accepted");
    put(L"SteamExe", L"");
    expect_failure<std::runtime_error>([] { static_cast<void>(steamfix::steam::registry_string(L"SteamExe")); },
                                       "empty registry string was accepted");
    const std::wstring oversized(70U * 1024U, L'x');
    put(L"SteamExe", oversized);
    expect_failure<std::runtime_error>([] { static_cast<void>(steamfix::steam::registry_string(L"SteamExe")); },
                                       "oversized registry string was accepted");
    const DWORD integer = 1;
    check_status(RegSetKeyValueW(key.get(), nullptr, L"SteamExe", REG_DWORD, &integer, sizeof(integer)));
    expect_failure<std::system_error>([] { static_cast<void>(steamfix::steam::registry_string(L"SteamExe")); },
                                      "wrong registry value type was accepted");
    constexpr std::array<unsigned char, 3> malformed{65, 0, 66};
    check_status(RegSetKeyValueW(key.get(), nullptr, L"SteamExe", REG_SZ, malformed.data(),
                                 static_cast<DWORD>(malformed.size())));
    expect_failure<std::runtime_error>([] { static_cast<void>(steamfix::steam::registry_string(L"SteamExe")); },
                                       "odd-byte registry string was accepted");
    const std::wstring variable = L"D2STEAMFIX_TEST_PATH_" + std::to_wstring(GetCurrentProcessId());
    const EnvironmentVariable environment(variable, (root / L"Steam").native());
    put(L"SteamPath", L"%" + variable + L"%", REG_EXPAND_SZ);
    require(steamfix::steam::registry_string(L"SteamPath") == (root / L"Steam").native(),
            "expandable registry path must expand process environment variables");
    put(L"SteamExe", (root / L"Steam" / L"steam.exe").native());
    put(L"SteamPath", (root / L"Steam").native());
    require(steamfix::steam::discover() == fs::canonical(root / L"Steam"), "Steam registry executable discovery failed");
    put(L"SteamExe", (root / L"does-not-exist.exe").native());
    require(steamfix::steam::discover() == fs::canonical(root / L"Steam"), "Steam registry path fallback failed");
    put(L"SteamPath", (root / L"does-not-exist").native());
    expect_failure<std::runtime_error>([] { static_cast<void>(steamfix::steam::discover()); },
                                       "Steam discovery accepted invalid registry locations");
}

void deny_execute(const HANDLE file) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(D;;0x20;;;WD)(A;;GA;;;WD)", SDDL_REVISION_1, &raw, nullptr) != FALSE,
            "could not create a test-only renderer ACL");
    const steamfix::LocalMemory descriptor(raw);
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL acl = nullptr;
    require(GetSecurityDescriptorDacl(raw, &present, &acl, &defaulted) != FALSE && present,
            "could not read test-only renderer ACL");
    check_status(static_cast<LSTATUS>(SetSecurityInfo(file, SE_FILE_OBJECT,
                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                 nullptr, nullptr, acl, nullptr)));
}

void check_repair_files(const fs::path& root, const steamfix::Logger& log) {
    using namespace steamfix::repair;
    require(missing(ERROR_FILE_NOT_FOUND) && missing(ERROR_PATH_NOT_FOUND) && !missing(ERROR_ACCESS_DENIED),
            "missing-file classification is incorrect");
    const fs::path directory = root / L"renderers";
    fs::create_directory(directory);
    require(!open_existing(directory / L"absent.dll", READ_CONTROL), "missing renderer must be optional");
    require(!needed(directory, log), "empty renderer directory cannot require permission repair");
    const fs::path renderer64 = directory / renderer_names.front();
    const fs::path renderer32 = directory / renderer_names.back();
    write_file(renderer64);
    auto file = open_existing(renderer64, READ_CONTROL | WRITE_DAC);
    require(file.has_value(), "regular renderer was not opened");
    require_regular(file->get(), renderer64);
    const UniqueHandle token = current_token();
    require(!token_user(token.get()).empty(), "token-user information was not returned");
    require(same_user(token.get(), token.get()), "same token must identify the same user");
    require(access_allowed(file->get(), token.get(), FILE_GENERIC_READ | FILE_GENERIC_EXECUTE),
            "new renderer should permit current-user read and execute");
    expect_failure<std::system_error>([&] { static_cast<void>(access_allowed(file->get(), nullptr, FILE_EXECUTE)); },
                                      "invalid access-check token was accepted");
    expect_failure<std::system_error>([&] { static_cast<void>(access_allowed(file->get(), token.get(), GENERIC_READ)); },
                                      "unmapped generic access request was accepted");
    require(!needed(directory, log), "usable renderer should not require repair");
    verify_repaired(directory, false, log);
    expect_failure<std::runtime_error>([&] { verify_repaired(directory, true, log); },
                                       "repair accepted a missing previously-present 32-bit renderer");
    write_file(renderer32);
    verify_repaired(directory, true, log);
    deny_execute(file->get());
    require(!access_allowed(file->get(), token.get(), FILE_EXECUTE), "explicit execute denial must be enforced");
    require(access_allowed(file->get(), token.get(), FILE_GENERIC_READ), "execute denial must preserve reading");
    require(needed(directory, log), "execute-denied renderer must require repair");
    expect_failure<std::runtime_error>([&] { verify_repaired(directory, true, log); },
                                       "repair accepted an execute-denied renderer");
    expect_failure<std::runtime_error>([] { static_cast<void>(token_user(nullptr)); }, "invalid token was accepted");
    for (const HANDLE invalid : std::array<HANDLE, 2>{nullptr, INVALID_HANDLE_VALUE}) {
        expect_windows_failure([&] { static_cast<void>(access_allowed(invalid, token.get(), FILE_EXECUTE)); },
                                ERROR_INVALID_HANDLE, "invalid access-check file did not report ERROR_INVALID_HANDLE");
    }
    expect_failure<std::system_error>([&] { require_regular(INVALID_HANDLE_VALUE, renderer64); },
                                      "invalid regular-file handle was accepted");
    const UniqueHandle folder(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    expect_failure<std::runtime_error>([&] { require_regular(folder.get(), directory); },
                                       "directory was accepted as a renderer file");
    const fs::path hardlink = directory / L"linked.dll";
    fs::create_hard_link(renderer32, hardlink);
    expect_failure<std::runtime_error>([&] { static_cast<void>(open_existing(renderer32, READ_CONTROL)); },
                                       "multiply-linked renderer was accepted");
    const fs::path symlink = directory / L"reparse.dll";
    if (CreateSymbolicLinkW(symlink.c_str(), renderer32.c_str(),
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE) {
        const UniqueHandle link(CreateFileW(symlink.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        expect_failure<std::runtime_error>([&] { require_regular(link.get(), symlink); },
                                           "reparse-point renderer was accepted");
        fs::remove(symlink);
    } else {
        std::cout << "Symbolic-link rejection requires Windows symbolic-link creation permission.\n";
    }
    expect_failure<std::runtime_error>([&] { static_cast<void>(remove_renderers(directory, log)); },
                                       "multiply-linked renderer deletion was accepted");
    require(fs::exists(renderer64), "renderer deletion started before both handles were validated");
    fs::remove(hardlink);
    file.reset();
    require(remove_renderers(directory, log), "existing 32-bit renderer must be recorded");
    require(!fs::exists(renderer64) && !fs::exists(renderer32), "renderer handles did not delete their files");
    require(!remove_renderers(directory, log), "absent 32-bit renderer must not be recorded");
    expect_failure<std::runtime_error>([&] { verify_repaired(directory, false, log); },
                                       "repair accepted a missing required 64-bit renderer");
    write_file(renderer64);
    require(!remove_renderers(directory, log), "64-bit-only renderer deletion result was incorrect");
    const fs::path recovery = directory / L"steamfix.dat";
    write_file(recovery);
    remove_file(recovery, log);
    require(!fs::exists(recovery), "repair recovery-file deletion failed");
    remove_file(recovery, log);
}

void check_mutexes() {
    using steamfix::repair::MutexLock;
    static_assert(!std::is_copy_constructible_v<MutexLock>);
    static_assert(!std::is_move_constructible_v<MutexLock>);
    static_assert(std::is_nothrow_destructible_v<MutexLock>);
    const std::wstring name = L"Local\\D2SteamFixTests-Mutex-" + std::to_wstring(GetCurrentProcessId());
    const UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const UniqueHandle release(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::exception_ptr failure;
    std::jthread owner([&] {
        try {
            const MutexLock lock(name.c_str(), 0);
            SetEvent(ready.get());
            require(WaitForSingleObject(release.get(), 10000) == WAIT_OBJECT_0, "mutex test release timed out");
        } catch (...) {
            failure = std::current_exception();
            SetEvent(ready.get());
        }
    });
    try {
        require(WaitForSingleObject(ready.get(), 5000) == WAIT_OBJECT_0, "mutex owner did not start");
        expect_failure<std::runtime_error>([&] { const MutexLock contender(name.c_str(), 0); },
                                           "contended repair mutex did not time out");
    } catch (...) {
        SetEvent(release.get());
        owner.join();
        throw;
    }
    SetEvent(release.get());
    owner.join();
    if (failure) std::rethrow_exception(failure);
    {
        const MutexLock reacquired(name.c_str(), 0);
    }

    const UniqueHandle abandoned(CreateMutexW(nullptr, FALSE, name.c_str()));
    bool acquired = false;
    std::jthread abandoning_owner([&] {
        acquired = WaitForSingleObject(abandoned.get(), 5000) == WAIT_OBJECT_0;
        // Thread exit deliberately abandons this test-owned mutex.
    });
    abandoning_owner.join();
    require(acquired, "test thread could not acquire its own mutex");
    const MutexLock recovered(name.c_str(), 0);
}

void check_processes(const fs::path& root, const steamfix::Logger& log) {
    using namespace steamfix::process;
    const fs::path self = image_path(GetCurrentProcess());
    require(fs::is_regular_file(self), "queried process image must exist");
    require(creation_key(GetCurrentProcess()) != 0, "process creation key must not be empty");
    const UniqueHandle query = open_query(GetCurrentProcessId());
    require(steamfix::paths::equal_windows(image_path(query.get()), self), "opened process image differs from self");
    const auto processes = snapshot();
    require(std::ranges::any_of(processes, [](const ProcessInfo& item) { return item.pid == GetCurrentProcessId(); }),
            "process snapshot omitted its caller");
    const auto parents = parent_chain();
    require(!parents.empty() && parents.size() <= 8U, "process parent chain is empty or exceeds its bound");
    require(std::ranges::find(parents, GetCurrentProcessId()) == parents.end(), "process parent chain includes self");
    require(process_name_is(L"DESTINY2.EXE", L"destiny2.exe"), "process name comparison must ignore case");
    require(!process_name_is(L"destiny2.exe.old", L"destiny2.exe"), "process name must match the whole value");
    require(process_message("start", L"destiny2.exe", 42).find("pid=42") != std::string::npos,
            "process diagnostic omitted its identity");
    expect_failure<std::system_error>([] { static_cast<void>(open_query(0)); }, "invalid process ID was accepted");
    expect_failure<std::system_error>([] { static_cast<void>(image_path(nullptr)); }, "invalid image-query handle was accepted");
    expect_failure<std::system_error>([] { static_cast<void>(creation_key(nullptr)); }, "invalid creation-time handle was accepted");
    expect_failure<std::system_error>([] { static_cast<void>(steamfix::repair::wait_for_process(nullptr, 0)); },
                                      "invalid process wait handle was accepted");
    require(matching_in_dir(root).empty(), "isolated directory unexpectedly contains a Destiny process");

    const fs::path launcher = root / L"destiny2launcher.exe";
    const fs::path game = root / L"destiny2.exe";
    fs::copy_file(self, launcher);
    fs::copy_file(self, game);
    {
        const OwnedChild launcher_child(launcher);
        const OwnedChild game_child(game);
        const auto matches = matching_in_dir(root);
        require(matches.size() == 2, "both test-owned Destiny categories must be discovered");
        require(std::ranges::any_of(matches, [&](const auto& item) { return item.first == launcher_child.pid(); }),
                "test-owned launcher identity was not preserved");
        require(std::ranges::any_of(matches, [&](const auto& item) { return item.first == game_child.pid(); }),
                "test-owned game identity was not preserved");
        require(matching_in_dir(root / L"different").empty(), "Destiny matching ignored executable directory");
    }
    const fs::path steam = root / L"steam.exe";
    fs::copy_file(self, steam);
    {
        OwnedChild child(steam);
        require(steamfix::repair::wait_for_process(child.get(), 0) == WAIT_TIMEOUT,
                "suspended child must remain alive");
        require(steamfix::repair::steam_client_alive(root), "test-owned Steam process was not discovered");
        require(!steamfix::repair::steam_client_alive(root / L"different"), "Steam liveness ignored installation directory");
        child.finish(37);
        require(steamfix::repair::wait_for_process(child.get(), 0) == WAIT_OBJECT_0,
                "finished child must signal its process handle");
        steamfix::repair::wait_for_repair(child.get(), root / L"different", log);
    }
    require(!steamfix::repair::steam_client_alive(root), "finished Steam child remained alive");
    expect_failure<std::system_error>([&] { steamfix::repair::wait_for_repair(nullptr, root, log); },
                                      "repair accepted an invalid child wait handle");
    require(!steamfix::overlay::loaded_renderer(), "isolated test executable unexpectedly loaded a Steam overlay");
}

struct FreeModule {
    void operator()(HMODULE module) const noexcept { FreeLibrary(module); }
};

void check_overlay_modules(const fs::path& root) {
    const fs::path fixture = steamfix::process::image_path(GetCurrentProcess()).parent_path() /
                             L"overlay_fixture.dll";
    require(fs::is_regular_file(fixture), "overlay test fixture DLL is missing beside the test executable");
    for (const auto* name : {L"GameOverlayRenderer64.dll", L"GameOverlayRenderer.dll"}) {
        const fs::path copy = root / name;
        fs::copy_file(fixture, copy);
        {
            const std::unique_ptr<std::remove_pointer_t<HMODULE>, FreeModule> loaded(LoadLibraryW(copy.c_str()));
            require(loaded != nullptr, "could not load the test-owned overlay fixture DLL");
            const auto found = steamfix::overlay::loaded_renderer();
            require(found && steamfix::paths::equal_windows(*found, copy),
                    "loaded renderer detection did not identify the test-owned module");
        }
        require(!steamfix::overlay::loaded_renderer(), "released overlay fixture remained loaded");
    }
}

} // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        const steamfix::Logger log(temporary.path() / L"steamfix.log");
        check_release_state();
        check_paths_and_installations(temporary.path());
        check_registry(temporary.path());
        check_repair_files(temporary.path(), log);
        check_mutexes();
        check_processes(temporary.path(), log);
        check_overlay_modules(temporary.path());
        std::cout << "Runtime safety, Steam discovery, process, repair, and overlay checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << "Runtime integration test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
