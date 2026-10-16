// Load native declarations first, then substitute only repair.cpp's external
// boundaries. File permissions, process identity, waits and termination remain
// real Windows operations on this test's own files and child processes.
#include "steamfix/repair.hpp"
#include "steamfix/command_line.hpp"
#include "steamfix/elevation.hpp"
#include "steamfix/error.hpp"
#include "steamfix/logging.hpp"
#include "steamfix/paths.hpp"
#include "steamfix/process.hpp"
#include "steamfix/steam.hpp"
#include "steamfix/ui.hpp"
#include "steamfix/win32.hpp"

#include <aclapi.h>
#include <cderr.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace repair_workflow {

namespace fs = std::filesystem;
using steamfix::UniqueHandle;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

[[noreturn]] void fail(const DWORD error) {
    throw std::system_error(steamfix::windows_exit_status(error), std::system_category());
}

void write_file(const fs::path& path, const std::string_view contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    stream.close();
    require(!stream.fail(), "write repair fixture file");
}

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "read repair fixture file");
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

fs::path executable_path() {
    return steamfix::process::image_path(GetCurrentProcess());
}

class TestDirectory {
public:
    TestDirectory() {
        GUID identifier{};
        require(SUCCEEDED(CoCreateGuid(&identifier)), "create repair fixture identifier");
        std::array<wchar_t, 40> text{};
        require(StringFromGUID2(identifier, text.data(), static_cast<int>(text.size())) != 0,
                "format repair fixture identifier");
        path_ = fs::temp_directory_path() /
                 (L"D2SteamFix repair tests " + std::wstring(text.data()));
        require(fs::create_directory(path_), "create repair fixture directory");
    }
    ~TestDirectory() noexcept {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;
    TestDirectory(TestDirectory&&) = delete;
    TestDirectory& operator=(TestDirectory&&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
private:
    fs::path path_;
};

enum class PickerKind { selection, cancel, failure };

struct PickerAction {
    PickerKind kind;
    fs::path path;
};

struct Plan {
    TestDirectory temporary;
    fs::path game = temporary.path() / L"game";
    fs::path steam = temporary.path() / L"Steam";
    fs::path recovery = game / L"steamfix.dat";
    std::unique_ptr<steamfix::Logger> logger;
    std::map<DWORD, fs::path> owned_processes;
    std::vector<UniqueHandle> process_lifetimes;
    std::vector<PickerAction> picker_actions;
    std::size_t picker_index = 0;
    DWORD picker_error = 0;
    int confirmation = IDOK;
    HRESULT confirmation_status = S_OK;
    bool discovery_failure = false;
    bool broker_cancelled = false;
    DWORD broker_error = ERROR_SUCCESS;
    bool broker_app_error = false;
    bool snapshot_failure = false;
    DWORD launch_error = ERROR_SUCCESS;
    bool desktop_launch = false;
    int confirmations = 0;
    int broker_calls = 0;
    int launches = 0;
    std::wstring launched_command;
    std::vector<std::wstring> information;
    std::vector<std::wstring> errors;
    bool message_recording_failed = false;
    // The watchdog joins before these fields or the temporary tree are destroyed.
    std::atomic<bool> watchdog_fired{false};
    std::jthread watchdog;

    explicit Plan(const bool renderer32 = true, const std::string_view recreation = "both") {
        fs::create_directory(game);
        fs::create_directory(steam);
        fs::copy_file(executable_path(), steam / L"steam.exe");
        write_file(steam / L".d2steamfix-test-owner",
                    "D2SteamFix repair fixture\n" + steamfix::narrow(fs::canonical(steam).native()) + "\n");
        write_file(steam / L".d2steamfix-test-recreation", recreation);
        write_file(steam / L"GameOverlayRenderer64.dll", "original64");
        if (renderer32) write_file(steam / L"GameOverlayRenderer.dll", "original32");
        for (const auto* name : {L"steamfix.dat", L"steamfix.dat.tmp", L"steamfix.dat.old"}) {
            write_file(game / name, "preserved recovery state");
        }
        logger = std::make_unique<steamfix::Logger>(game / L"steamfix.log");
        logger->info("repair workflow fixture log must survive repair");
    }

    [[nodiscard]] const steamfix::Logger& log() const {
        if (!logger) throw std::runtime_error("repair fixture logger is absent");
        return *logger;
    }
    [[nodiscard]] bool owns(const fs::path& path) const {
        const fs::path relative = fs::weakly_canonical(path).lexically_relative(fs::canonical(temporary.path()));
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != L"..";
    }
    [[nodiscard]] bool owns_pid(const DWORD pid) const noexcept {
        return pid == GetCurrentProcessId() || owned_processes.contains(pid);
    }
};

// Scope is confined to the calling test thread. Separately compiled production
// dependencies and the child executable retain their original native bindings.
thread_local Plan* active = nullptr;

class Scope {
public:
    explicit Scope(Plan& plan) {
        require(active == nullptr, "nested repair fixture scope");
        active = &plan;
    }
    ~Scope() noexcept { active = nullptr; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
};

UniqueHandle duplicate_process(const HANDLE process) {
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &duplicate,
                          0, FALSE, DUPLICATE_SAME_ACCESS)) fail(GetLastError());
    return UniqueHandle(duplicate);
}

class OwnedChild {
public:
    OwnedChild(Plan& plan, const fs::path& executable) {
        require(plan.owns(executable), "child executable escaped repair fixture");
        fs::create_directories(executable.parent_path());
        if (!fs::exists(executable)) fs::copy_file(executable_path(), executable);
        std::wstring command;
        steamfix::command_line::append_quoted(command, executable.native());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                              CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                              executable.parent_path().c_str(), &startup, &information)) fail(GetLastError());
        process_ = UniqueHandle(information.hProcess);
        thread_ = UniqueHandle(information.hThread);
        try {
            plan.owned_processes.emplace(information.dwProcessId, fs::canonical(executable));
            plan.process_lifetimes.push_back(duplicate_process(process_.get()));
        } catch (...) {
            ::TerminateProcess(process_.get(), 1);
            ::WaitForSingleObject(process_.get(), 5000);
            throw;
        }
    }
    ~OwnedChild() noexcept {
        if (process_.get() != nullptr && ::WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
            ::TerminateProcess(process_.get(), 1);
            ::WaitForSingleObject(process_.get(), 5000);
        }
    }
    OwnedChild(const OwnedChild&) = delete;
    OwnedChild& operator=(const OwnedChild&) = delete;
    OwnedChild(OwnedChild&&) = delete;
    OwnedChild& operator=(OwnedChild&&) = delete;
    [[nodiscard]] bool alive() const noexcept { return ::WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
    void require_repair_shutdown() const {
        require(::WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0, "repair left its owned target alive");
        DWORD code = 0;
        if (!GetExitCodeProcess(process_.get(), &code)) fail(GetLastError());
        require(code == ERROR_CANCELLED, "repair did not use its documented shutdown status");
    }
private:
    UniqueHandle process_;
    UniqueHandle thread_;
};

HANDLE WINAPI snapshot(const DWORD flags, const DWORD pid) {
    if (active != nullptr) {
        require(flags == TH32CS_SNAPPROCESS && pid == 0, "unexpected repair snapshot scope");
        if (active->snapshot_failure) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return INVALID_HANDLE_VALUE;
        }
    }
    return ::CreateToolhelp32Snapshot(flags, pid);
}

BOOL WINAPI first_process(const HANDLE snapshot_handle, const LPPROCESSENTRY32W entry) {
    BOOL found = ::Process32FirstW(snapshot_handle, entry);
    while (found && active != nullptr && !active->owns_pid(entry->th32ProcessID)) {
        found = ::Process32NextW(snapshot_handle, entry);
    }
    return found;
}

BOOL WINAPI next_process(const HANDLE snapshot_handle, const LPPROCESSENTRY32W entry) {
    BOOL found = ::Process32NextW(snapshot_handle, entry);
    while (found && active != nullptr && !active->owns_pid(entry->th32ProcessID)) {
        found = ::Process32NextW(snapshot_handle, entry);
    }
    return found;
}

HANDLE WINAPI create_mutex(LPSECURITY_ATTRIBUTES security, const BOOL owner, const LPCWSTR name) {
    if (active == nullptr) return ::CreateMutexW(security, owner, name);
    require(name != nullptr, "unnamed production repair mutex");
    const std::wstring_view requested(name);
    require(requested == L"Global\\D2SteamFix-Repair-1085660" || requested == L"Global\\D2SteamFix-1085660",
            "unexpected production repair mutex name");
    const std::wstring isolated = L"Local\\D2SteamFix-RepairTest-" +
        active->temporary.path().filename().native() + L"-" +
        (requested == L"Global\\D2SteamFix-Repair-1085660" ? L"repair" : L"session");
    return ::CreateMutexW(security, owner, isolated.c_str());
}

HRESULT WINAPI task_dialog(const TASKDIALOGCONFIG* dialog, int* selected, int* radio, BOOL* checked) {
    if (active == nullptr) return ::TaskDialogIndirect(dialog, selected, radio, checked);
    if (dialog == nullptr || selected == nullptr) throw std::runtime_error("repair confirmation omitted its configuration");
    require(dialog->nDefaultButton == IDCANCEL && dialog->cButtons == 2 &&
            (dialog->dwFlags & TDF_ALLOW_DIALOG_CANCELLATION) != 0, "repair confirmation lost safe cancellation defaults");
    require(std::wstring_view(dialog->pszMainInstruction).find(L"Steam and Destiny 2 will close") != std::wstring_view::npos,
            "repair confirmation did not disclose application shutdown");
    ++active->confirmations;
    *selected = active->confirmation;
    return active->confirmation_status;
}

BOOL WINAPI pick_file(LPOPENFILENAMEW picker) {
    if (active == nullptr) return ::GetOpenFileNameW(picker);
    if (picker == nullptr || picker->lpstrFile == nullptr) throw std::runtime_error("repair picker omitted its output buffer");
    require((picker->Flags & (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR)) ==
            (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR), "repair picker lost safe path-selection flags");
    require(active->picker_index < active->picker_actions.size(), "unexpected extra repair file-picker request");
    const PickerAction& action = active->picker_actions.at(active->picker_index++);
    active->picker_error = 0;
    if (action.kind == PickerKind::cancel) return FALSE;
    if (action.kind == PickerKind::failure) {
        active->picker_error = CDERR_INITIALIZATION;
        return FALSE;
    }
    require(active->owns(action.path), "picker selection escaped repair fixture");
    const std::wstring selected = action.path.native();
    require(selected.size() < picker->nMaxFile, "repair picker output buffer is too small");
    std::copy(selected.begin(), selected.end(), picker->lpstrFile);
    picker->lpstrFile[selected.size()] = L'\0';
    return TRUE;
}

DWORD WINAPI picker_error() {
    return active != nullptr ? active->picker_error : ::CommDlgExtendedError();
}

BOOL WINAPI create_process(LPCWSTR executable, LPWSTR command, LPSECURITY_ATTRIBUTES process_security,
                           LPSECURITY_ATTRIBUTES thread_security, const BOOL inherit, const DWORD flags,
                           LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup,
                           LPPROCESS_INFORMATION information) {
    if (active == nullptr) {
        return ::CreateProcessW(executable, command, process_security, thread_security, inherit,
                                 flags, environment, directory, startup, information);
    }
    if (executable == nullptr || directory == nullptr || command == nullptr || information == nullptr) {
        throw std::runtime_error("repair launch omitted its explicit executable, working directory or process output");
    }
    require(active->owns(fs::path(executable)) &&
            steamfix::paths::equal_windows(fs::path(executable), active->steam / L"steam.exe") &&
            steamfix::paths::equal_windows(fs::path(directory), active->steam), "repair child launch escaped fixture");
    require(std::wstring_view(command).find(L" -forcesteamupdate -forcepackagedownload -exitsteam") != std::wstring_view::npos,
            "repair launch omitted its actual Steam repair flags");
    ++active->launches;
    active->launched_command = command;
    if (active->launch_error != ERROR_SUCCESS) {
        SetLastError(active->launch_error);
        return FALSE;
    }
    if (!::CreateProcessW(executable, command, process_security, thread_security, inherit,
                          flags | CREATE_NO_WINDOW, environment, directory, startup, information)) return FALSE;
    UniqueHandle process(information->hProcess);
    UniqueHandle thread(information->hThread);
    try {
        active->owned_processes.emplace(information->dwProcessId, fs::canonical(fs::path(executable)));
        active->process_lifetimes.push_back(duplicate_process(process.get()));
        UniqueHandle watched = duplicate_process(process.get());
        Plan* const plan = active;
        active->watchdog = std::jthread([plan, watched = std::move(watched)] {
            if (::WaitForSingleObject(watched.get(), 8000) == WAIT_TIMEOUT) {
                plan->watchdog_fired.store(true);
                ::TerminateProcess(watched.get(), 98);
                ::WaitForSingleObject(watched.get(), 5000);
            }
        });
    } catch (...) {
        ::TerminateProcess(process.get(), 1);
        ::WaitForSingleObject(process.get(), 5000);
        throw;
    }
    information->hProcess = process.release();
    information->hThread = thread.release();
    return TRUE;
}

} // namespace repair_workflow

namespace steamfix::repair_fixture_steam {
std::filesystem::path discover() {
    if (repair_workflow::active == nullptr) return steamfix::steam::discover();
    if (repair_workflow::active->discovery_failure) throw std::runtime_error("controlled fixture discovery failure");
    return repair_workflow::active->steam;
}
} // namespace steamfix::repair_fixture_steam

namespace steamfix::repair_fixture_elevation {

UniqueHandle open_file(const std::filesystem::path& path, const DWORD access, const DWORD share,
                        const DWORD creation, const DWORD flags, SECURITY_ATTRIBUTES* security = nullptr) {
    if (repair_workflow::active == nullptr) return steamfix::elevation::open_file(path, access, share, creation, flags, security);
    repair_workflow::require(repair_workflow::active->owns(path), "repair file access escaped fixture");
    // Use the same native first attempt, without any possible broker/UAC fallback.
    return UniqueHandle(::CreateFileW(path.c_str(), access, share, security, creation, flags, nullptr));
}

UniqueHandle open_process_for_termination(const DWORD pid) {
    if (repair_workflow::active == nullptr) return steamfix::elevation::open_process_for_termination(pid);
    const auto found = repair_workflow::active->owned_processes.find(pid);
    if (found == repair_workflow::active->owned_processes.end()) throw std::runtime_error("shutdown attempted an unowned process ID");
    UniqueHandle handle(::OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    const auto actual = steamfix::process::image_path(handle.get());
    repair_workflow::require(repair_workflow::active->owns(actual) && steamfix::paths::equal_windows(actual, found->second),
                             "shutdown process identity no longer matches its owned executable");
    return handle;
}

void ensure_broker() {
    if (repair_workflow::active == nullptr) {
        steamfix::elevation::ensure_broker();
        return;
    }
    ++repair_workflow::active->broker_calls;
    if (repair_workflow::active->broker_cancelled) throw steamfix::elevation::Cancelled();
    if (repair_workflow::active->broker_error != ERROR_SUCCESS) repair_workflow::fail(repair_workflow::active->broker_error);
    if (repair_workflow::active->broker_app_error) throw AppError(ExitCode::validation, "fixture broker", "controlled application error");
}

bool is_elevated() {
    return repair_workflow::active != nullptr ? false : steamfix::elevation::is_elevated();
}

bool launch_as_desktop_user(const std::filesystem::path& executable, const std::wstring& command,
                            const std::filesystem::path& directory, PROCESS_INFORMATION& information) {
    if (repair_workflow::active == nullptr) {
        return steamfix::elevation::launch_as_desktop_user(executable, command, directory, information);
    }
    if (!repair_workflow::active->desktop_launch) return false;
    std::wstring mutable_command(command);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!repair_workflow::create_process(executable.c_str(), mutable_command.data(), nullptr, nullptr,
                                         FALSE, 0, nullptr, directory.c_str(), &startup, &information)) {
        repair_workflow::fail(GetLastError());
    }
    return true;
}

} // namespace steamfix::repair_fixture_elevation

// Headless UI endpoints record only messages produced during an active fixture.
namespace steamfix::ui {
void info(const std::wstring_view message) noexcept {
    if (repair_workflow::active == nullptr) return;
    try { repair_workflow::active->information.emplace_back(message); }
    catch (...) { repair_workflow::active->message_recording_failed = true; }
}
void error(const std::wstring_view message) noexcept {
    if (repair_workflow::active == nullptr) return;
    try { repair_workflow::active->errors.emplace_back(message); }
    catch (...) { repair_workflow::active->message_recording_failed = true; }
}
} // namespace steamfix::ui

#define TaskDialogIndirect repair_workflow::task_dialog
#define GetOpenFileNameW repair_workflow::pick_file
#define CommDlgExtendedError repair_workflow::picker_error
#define CreateToolhelp32Snapshot repair_workflow::snapshot
#define Process32FirstW repair_workflow::first_process
#define Process32NextW repair_workflow::next_process
#define CreateMutexW repair_workflow::create_mutex
#define CreateProcessW repair_workflow::create_process
#define steam repair_fixture_steam
#define elevation repair_fixture_elevation
#include "../src/repair.cpp"
#undef elevation
#undef steam
#undef CreateProcessW
#undef CreateMutexW
#undef Process32NextW
#undef Process32FirstW
#undef CreateToolhelp32Snapshot
#undef CommDlgExtendedError
#undef GetOpenFileNameW
#undef TaskDialogIndirect

namespace repair_workflow {

void require_recovery(const Plan& plan, const bool present) {
    for (const auto* name : {L"steamfix.dat", L"steamfix.dat.tmp", L"steamfix.dat.old"}) {
        require(fs::exists(plan.game / name) == present, "repair recovery artifact preservation/cleanup was incorrect");
    }
    require(fs::is_regular_file(plan.game / L"steamfix.log") && fs::file_size(plan.game / L"steamfix.log") != 0,
            "repair removed or truncated its log");
    require(!plan.watchdog_fired.load(), "repair child exceeded its eight-second fixture watchdog");
    require(!plan.message_recording_failed, "headless repair message recording failed");
}

template<std::invocable Operation>
void expect_repair_failure(Operation operation, const steamfix::ExitCode expected = steamfix::ExitCode::overlay_protection) {
    try {
        operation();
    } catch (const steamfix::AppError& error) {
        require(error.code() == expected, "repair failure returned an unexpected exit category");
        return;
    }
    throw std::runtime_error("expected repair failure was accepted");
}

void check_cancellation() {
    {
        Plan plan;
        const OwnedChild game(plan, plan.game / L"destiny2.exe");
        const OwnedChild steam(plan, plan.steam / L"steam.exe");
        plan.confirmation = IDCANCEL;
        const Scope scope(plan);
        require(!steamfix::repair::run(plan.game, plan.recovery, plan.log()), "declined repair consent was ignored");
        require(game.alive() && steam.alive(), "consent cancellation closed a fixture application");
        require(read_file(plan.steam / L"GameOverlayRenderer64.dll") == "original64", "consent cancellation modified renderer");
        require(plan.confirmations == 1 && plan.broker_calls == 0 && plan.launches == 0, "cancelled repair continued into broker/launch");
        require_recovery(plan, true);
    }
    {
        Plan plan;
        const OwnedChild game(plan, plan.game / L"destiny2.exe");
        const OwnedChild steam(plan, plan.steam / L"steam.exe");
        plan.broker_cancelled = true;
        const Scope scope(plan);
        require(!steamfix::repair::run(plan.game, plan.recovery, plan.log()), "broker cancellation was ignored");
        require(game.alive() && steam.alive(), "broker cancellation closed a fixture application");
        require(plan.broker_calls == 1 && plan.launches == 0, "cancelled broker continued into repair launch");
        require(read_file(plan.steam / L"GameOverlayRenderer64.dll") == "original64", "broker cancellation modified renderer");
        require_recovery(plan, true);
    }
}

void check_success(const bool renderer32, const bool desktop_launch, const bool use_picker) {
    Plan plan(renderer32, renderer32 ? "both" : "64");
    const OwnedChild game(plan, plan.game / L"destiny2.exe");
    const OwnedChild launcher(plan, plan.game / L"destiny2launcher.exe");
    const OwnedChild steam(plan, plan.steam / L"steam.exe");
    const OwnedChild unrelated_service(plan, plan.temporary.path() / L"unrelated" / L"steamservice.exe");
    plan.desktop_launch = desktop_launch;
    plan.discovery_failure = use_picker;
    if (use_picker) {
        plan.picker_actions = {{PickerKind::selection, plan.steam / L"not-steam.exe"},
                               {PickerKind::selection, plan.steam / L"steam.exe"}};
    }
    const Scope scope(plan);
    require(steamfix::repair::run(plan.game, plan.recovery, plan.log()), "owned fixture repair did not succeed");
    game.require_repair_shutdown();
    launcher.require_repair_shutdown();
    steam.require_repair_shutdown();
    require(unrelated_service.alive(), "repair closed an unrelated fixture service outside Steam installation");
    require(read_file(plan.steam / L"GameOverlayRenderer64.dll") == "repaired64", "repair did not recreate 64-bit renderer");
    if (renderer32) require(read_file(plan.steam / L"GameOverlayRenderer.dll") == "repaired32", "repair did not recreate 32-bit renderer");
    else require(!fs::exists(plan.steam / L"GameOverlayRenderer.dll"), "repair created an unrequested optional renderer");
    require(plan.confirmations == 1 && plan.broker_calls == 1 && plan.launches == 1, "successful repair repeated or skipped a workflow stage");
    require(plan.information.size() == 1 && plan.information.front().find(L"Steam repaired its files") != std::wstring::npos,
            "successful repair did not report completion");
    require(plan.picker_index == (use_picker ? 2U : 0U) && plan.errors.size() == (use_picker ? 1U : 0U),
            "repair picker did not reject invalid selection and accept valid Steam executable");
    require_recovery(plan, false);
}

void check_failures() {
    {
        Plan plan;
        const Scope scope(plan);
        expect_repair_failure([&] { static_cast<void>(steamfix::repair::run(plan.game, plan.game / L"wrong.dat", plan.log())); });
        require(plan.confirmations == 0 && plan.launches == 0, "invalid recovery path reached consent or launch");
        require_recovery(plan, true);
    }
    {
        Plan plan;
        plan.confirmation_status = E_FAIL;
        const Scope scope(plan);
        expect_repair_failure([&] { static_cast<void>(steamfix::repair::run(plan.game, plan.recovery, plan.log())); });
        require(plan.broker_calls == 0, "failed consent display reached broker");
        require_recovery(plan, true);
    }
    {
        Plan plan;
        plan.broker_error = ERROR_ACCESS_DENIED;
        const Scope scope(plan);
        expect_repair_failure([&] { static_cast<void>(steamfix::repair::run(plan.game, plan.recovery, plan.log())); });
        require(plan.launches == 0, "failed broker reached child launch");
        require_recovery(plan, true);
    }
    {
        Plan plan;
        plan.broker_app_error = true;
        const Scope scope(plan);
        expect_repair_failure([&] { static_cast<void>(steamfix::repair::run(plan.game, plan.recovery, plan.log())); },
                               steamfix::ExitCode::validation);
        require_recovery(plan, true);
    }
    {
        Plan plan;
        const OwnedChild game(plan, plan.game / L"destiny2.exe");
        plan.snapshot_failure = true;
        const Scope scope(plan);
        expect_repair_failure([&] { static_cast<void>(steamfix::repair::run(plan.game, plan.recovery, plan.log())); });
        require(game.alive() && plan.launches == 0, "snapshot failure closed a process or launched repair");
        require(read_file(plan.steam / L"GameOverlayRenderer64.dll") == "original64", "snapshot failure modified renderer");
        require_recovery(plan, true);
    }
    {
        Plan plan;
        plan.launch_error = ERROR_ACCESS_DENIED;
        const Scope scope(plan);
        expect_repair_failure([&] { static_cast<void>(steamfix::repair::run(plan.game, plan.recovery, plan.log())); });
        require(plan.launches == 1 && plan.information.empty(), "failed repair launch reported completion");
        require_recovery(plan, true);
    }
    for (const auto mode : {"none", "64"}) {
        Plan plan(true, mode);
        const Scope scope(plan);
        expect_repair_failure([&] { static_cast<void>(steamfix::repair::run(plan.game, plan.recovery, plan.log())); });
        require(plan.launches == 1 && plan.information.empty(), "failed renderer recreation reported completion");
        require_recovery(plan, true);
    }
}

void check_picker_cancellation_and_failure() {
    {
        Plan plan;
        plan.discovery_failure = true;
        plan.picker_actions = {{PickerKind::cancel, {}}};
        const Scope scope(plan);
        require(!steamfix::repair::run(plan.game, plan.recovery, plan.log()), "cancelled Steam picker continued repair");
        require(plan.confirmations == 0 && plan.broker_calls == 0, "cancelled Steam picker reached consent or broker");
        require_recovery(plan, true);
    }
    {
        Plan plan;
        plan.discovery_failure = true;
        plan.picker_actions = {{PickerKind::failure, {}}};
        const Scope scope(plan);
        expect_repair_failure([&] { static_cast<void>(steamfix::repair::run(plan.game, plan.recovery, plan.log())); });
        require(plan.confirmations == 0 && plan.broker_calls == 0, "failed Steam picker reached consent or broker");
        require_recovery(plan, true);
    }
}

int recreate_owned_renderers(const std::span<wchar_t*> arguments) {
    if (arguments.size() != 4 || std::wstring_view(arguments[1]) != L"-forcesteamupdate" ||
        std::wstring_view(arguments[2]) != L"-forcepackagedownload" || std::wstring_view(arguments[3]) != L"-exitsteam") return 90;
    const fs::path directory = fs::canonical(fs::current_path());
    if (!directory.parent_path().filename().native().starts_with(L"D2SteamFix repair tests ") ||
        !steamfix::paths::equal_windows(executable_path(), directory / L"steam.exe")) return 91;
    const std::string marker = "D2SteamFix repair fixture\n" + steamfix::narrow(directory.native()) + "\n";
    if (read_file(directory / L".d2steamfix-test-owner") != marker) return 92;
    const std::string mode = read_file(directory / L".d2steamfix-test-recreation");
    if (mode == "none") return 42;
    if (mode != "both" && mode != "64") return 93;
    write_file(directory / L"GameOverlayRenderer64.dll", "repaired64");
    if (mode == "both") write_file(directory / L"GameOverlayRenderer.dll", "repaired32");
    return 0;
}

struct CommandLineDeleter {
    void operator()(wchar_t** arguments) const noexcept { LocalFree(arguments); }
};

} // namespace repair_workflow

int main() {
    try {
        int count = 0;
        const std::unique_ptr<wchar_t*, repair_workflow::CommandLineDeleter> arguments(CommandLineToArgvW(GetCommandLineW(), &count));
        if (!arguments || count <= 0) throw std::runtime_error("could not read repair workflow arguments");
        const std::span parsed{arguments.get(), static_cast<std::size_t>(count)};
        if (steamfix::paths::name_is(repair_workflow::executable_path(), L"steam.exe")) {
            return repair_workflow::recreate_owned_renderers(parsed);
        }
        repair_workflow::require(count == 1, "unexpected repair workflow test arguments");
        repair_workflow::check_cancellation();
        repair_workflow::check_picker_cancellation_and_failure();
        repair_workflow::check_success(true, false, false);
        repair_workflow::check_success(false, true, true);
        repair_workflow::check_failures();
        std::cout << "Repair workflow cancellation, owned process shutdown, launch, verification and cleanup passed.\n";
    } catch (const std::exception& error) {
        std::cerr << "Repair workflow test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
