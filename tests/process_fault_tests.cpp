// Load every production dependency before rewriting only the API call sites in
// process.cpp. Windows declarations and the separately compiled dependencies
// always retain their real implementations.
#include "steamfix/process.hpp"
#include "steamfix/logging.hpp"
#include "steamfix/paths.hpp"
#include "steamfix/command_line.hpp"

#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace process_fault {

struct FaultPlan {
    HANDLE child = nullptr;
    HANDLE game = nullptr;
    DWORD child_pid = 0;
    DWORD candidate_pid = 0;
    int snapshot_creation_remaining = 0;
    int first_entry_remaining = 0;
    int next_entry_remaining = 0;
    int child_wait_remaining = 0;
    int candidate_open_remaining = 0;
    int exit_status_remaining = 0;
    int snapshot_creation_failures = 0;
    int first_entry_failures = 0;
    int next_entry_failures = 0;
    int child_wait_failures = 0;
    int candidate_open_failures = 0;
    int exit_status_failures = 0;
    int real_candidate_opens = 0;
    bool snapshot_contains_owned_process = false;
    bool snapshot_completed = false;
    bool game_confirmed_this_poll = false;
    bool reliable_absence_completed = false;
    bool post_exit_failure_fired = false;
    int post_failure_absent_snapshots = 0;
    std::chrono::steady_clock::time_point post_exit_failure_at{};
    std::atomic<bool> fail_snapshot_after_exit{false};
    steamfix::UniqueHandle candidate_verified{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

// Only the monitor thread reads these finite budgets and counters. The helper
// thread communicates through the atomic flag and native event alone.
thread_local FaultPlan* active_plan = nullptr;

bool consume(int& budget) noexcept {
    if (budget == 0) return false;
    --budget;
    return true;
}

HANDLE WINAPI create_snapshot(const DWORD flags, const DWORD pid) {
    if (active_plan != nullptr) {
        active_plan->snapshot_contains_owned_process = false;
        active_plan->snapshot_completed = false;
        if (active_plan->fail_snapshot_after_exit.load() && active_plan->reliable_absence_completed &&
            ::WaitForSingleObject(active_plan->child, 0) == WAIT_OBJECT_0 &&
            ::WaitForSingleObject(active_plan->game, 0) == WAIT_OBJECT_0 &&
            active_plan->fail_snapshot_after_exit.exchange(false)) {
            ++active_plan->snapshot_creation_failures;
            active_plan->post_exit_failure_fired = true;
            active_plan->post_exit_failure_at = std::chrono::steady_clock::now();
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return INVALID_HANDLE_VALUE;
        }
        if (consume(active_plan->snapshot_creation_remaining)) {
            ++active_plan->snapshot_creation_failures;
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return INVALID_HANDLE_VALUE;
        }
    }
    return ::CreateToolhelp32Snapshot(flags, pid);
}

BOOL WINAPI first_entry(const HANDLE snapshot, const LPPROCESSENTRY32W entry) {
    if (active_plan != nullptr && consume(active_plan->first_entry_remaining)) {
        ++active_plan->first_entry_failures;
        SetLastError(ERROR_BAD_LENGTH);
        return FALSE;
    }
    const BOOL result = ::Process32FirstW(snapshot, entry);
    if (result && active_plan != nullptr &&
        (entry->th32ProcessID == active_plan->child_pid || entry->th32ProcessID == active_plan->candidate_pid)) {
        active_plan->snapshot_contains_owned_process = true;
    }
    return result;
}

BOOL WINAPI next_entry(const HANDLE snapshot, const LPPROCESSENTRY32W entry) {
    if (active_plan != nullptr && consume(active_plan->next_entry_remaining)) {
        ++active_plan->next_entry_failures;
        SetLastError(ERROR_BAD_LENGTH);
        return FALSE;
    }
    const BOOL result = ::Process32NextW(snapshot, entry);
    const DWORD error = GetLastError();
    if (active_plan != nullptr) {
        if (result && (entry->th32ProcessID == active_plan->child_pid ||
                       entry->th32ProcessID == active_plan->candidate_pid)) {
            active_plan->snapshot_contains_owned_process = true;
        }
        if (!result && error == ERROR_NO_MORE_FILES) {
            active_plan->snapshot_completed = true;
            if (active_plan->post_exit_failure_fired && !active_plan->snapshot_contains_owned_process &&
                ::WaitForSingleObject(active_plan->child, 0) == WAIT_OBJECT_0 &&
                ::WaitForSingleObject(active_plan->game, 0) == WAIT_OBJECT_0) {
                ++active_plan->post_failure_absent_snapshots;
            }
        }
    }
    SetLastError(error);
    return result;
}

DWORD WINAPI wait_for_child(const HANDLE handle, const DWORD timeout) {
    if (active_plan != nullptr && handle == active_plan->child &&
        consume(active_plan->child_wait_remaining)) {
        ++active_plan->child_wait_failures;
        SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    return ::WaitForSingleObject(handle, timeout);
}

HANDLE WINAPI open_candidate(const DWORD access, const BOOL inherit, const DWORD pid) {
    if (active_plan != nullptr && pid == active_plan->candidate_pid &&
        consume(active_plan->candidate_open_remaining)) {
        ++active_plan->candidate_open_failures;
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    const HANDLE handle = ::OpenProcess(access, inherit, pid);
    if (active_plan != nullptr && pid == active_plan->candidate_pid && handle != nullptr) {
        ++active_plan->real_candidate_opens;
    }
    return handle;
}

BOOL WINAPI exit_code(const HANDLE handle, const LPDWORD status) {
    if (active_plan != nullptr && handle == active_plan->child &&
        ::WaitForSingleObject(handle, 0) == WAIT_OBJECT_0 &&
        consume(active_plan->exit_status_remaining)) {
        ++active_plan->exit_status_failures;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::GetExitCodeProcess(handle, status);
}

BOOL WINAPI process_times(const HANDLE handle, const LPFILETIME created, const LPFILETIME exited,
                          const LPFILETIME kernel, const LPFILETIME user) {
    const BOOL result = ::GetProcessTimes(handle, created, exited, kernel, user);
    const DWORD error = GetLastError();
    if (result && active_plan != nullptr && ::GetProcessId(handle) == active_plan->candidate_pid) {
        // Production reaches creation_key only after the real image path was
        // verified inside this scenario's unique directory.
        active_plan->game_confirmed_this_poll = true;
    }
    SetLastError(error);
    return result;
}

void WINAPI end_poll(const DWORD milliseconds) {
    if (active_plan != nullptr) {
        if (active_plan->game_confirmed_this_poll) {
            active_plan->game_confirmed_this_poll = false;
            SetEvent(active_plan->candidate_verified.get());
        }
        if (active_plan->snapshot_completed && !active_plan->snapshot_contains_owned_process &&
            ::WaitForSingleObject(active_plan->child, 0) == WAIT_OBJECT_0 &&
            ::WaitForSingleObject(active_plan->game, 0) == WAIT_OBJECT_0) {
            // This boundary follows ReleaseState::observe. Inject the next
            // failure after one actual all-absent poll has been accumulated.
            active_plan->reliable_absence_completed = true;
        }
    }
    ::Sleep(milliseconds);
}

class Scope final {
public:
    explicit Scope(FaultPlan& plan) noexcept : previous_(active_plan) { active_plan = &plan; }
    ~Scope() noexcept { active_plan = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
private:
    FaultPlan* previous_;
};

} // namespace process_fault

#define CreateToolhelp32Snapshot process_fault::create_snapshot
#define Process32FirstW process_fault::first_entry
#define Process32NextW process_fault::next_entry
#define WaitForSingleObject process_fault::wait_for_child
#define OpenProcess process_fault::open_candidate
#define GetExitCodeProcess process_fault::exit_code
#define GetProcessTimes process_fault::process_times
#define Sleep process_fault::end_poll
#include "../src/process.cpp"
#undef CreateToolhelp32Snapshot
#undef Process32FirstW
#undef Process32NextW
#undef WaitForSingleObject
#undef OpenProcess
#undef GetExitCodeProcess
#undef GetProcessTimes
#undef Sleep

namespace {

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ = fs::temp_directory_path() /
                (L"D2SteamFix-process-faults-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (fs::create_directory(path_)) return;
        }
        throw std::runtime_error("could not create an isolated process test directory");
    }
    ~TemporaryDirectory() noexcept {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    const fs::path& path() const noexcept { return path_; }
private:
    fs::path path_;
};

class OwnedChild final {
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
            throw std::system_error(steamfix::windows_exit_status(GetLastError()), std::system_category());
        }
        process_.reset(information.hProcess);
        thread_.reset(information.hThread);
        pid_ = information.dwProcessId;
    }
    ~OwnedChild() noexcept { static_cast<void>(finish(0)); }
    OwnedChild(const OwnedChild&) = delete;
    OwnedChild& operator=(const OwnedChild&) = delete;
    OwnedChild(OwnedChild&&) = delete;
    OwnedChild& operator=(OwnedChild&&) = delete;
    HANDLE get() const noexcept { return process_.get(); }
    DWORD pid() const noexcept { return pid_; }
    bool finish(const DWORD status) const noexcept {
        // Termination is restricted to this fixture's exact CreateProcess handle.
        if (::WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0) return true;
        if (!TerminateProcess(process_.get(), status)) return false;
        return ::WaitForSingleObject(process_.get(), 5000) == WAIT_OBJECT_0;
    }
private:
    steamfix::UniqueHandle process_;
    steamfix::UniqueHandle thread_;
    DWORD pid_ = 0;
};

std::string read_log(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "could not inspect process test log");
    const std::string result{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    require(!file.bad(), "process test log read failed");
    return result;
}

void require_contains(const std::string& log, const char* message) {
    require(log.find(message) != std::string::npos, message);
}

void run_scenario(const bool unavailable_exit_status) {
    TemporaryDirectory directory;
    const fs::path self = steamfix::process::image_path(GetCurrentProcess());
    const fs::path launcher_path = directory.path() / L"destiny2launcher.exe";
    const fs::path game_path = directory.path() / L"destiny2.exe";
    require(fs::copy_file(self, launcher_path), "could not copy the launcher fixture");
    require(fs::copy_file(self, game_path), "could not copy the game fixture");
    OwnedChild launcher(launcher_path);
    OwnedChild game(game_path);
    process_fault::FaultPlan plan;
    plan.child = launcher.get();
    plan.game = game.get();
    plan.child_pid = launcher.pid();
    plan.candidate_pid = game.pid();
    if (unavailable_exit_status) {
        plan.exit_status_remaining = 1;
    } else {
        plan.snapshot_creation_remaining = 1;
        plan.first_entry_remaining = 1;
        plan.next_entry_remaining = 1;
        plan.child_wait_remaining = 1;
        plan.candidate_open_remaining = 1;
    }
    std::atomic<bool> worker_ok{true};
    constexpr DWORD launcher_status = 0x1234;
    const fs::path log_path = directory.path() / L"steamfix.log";
    std::optional<int> result;
    bool children_exited_when_monitor_returned = false;
    std::chrono::steady_clock::time_point completed_at{};
    {
        const steamfix::Logger log(log_path);
        // The helper always finishes both owned children after a bounded wait,
        // including if a regression prevents their successful observation.
        std::jthread finisher([&] {
            if (::WaitForSingleObject(plan.candidate_verified.get(), 15000) != WAIT_OBJECT_0) {
                worker_ok.store(false);
            }
            if (!unavailable_exit_status) plan.fail_snapshot_after_exit.store(true);
            if (!launcher.finish(launcher_status)) worker_ok.store(false);
            if (!game.finish(0)) worker_ok.store(false);
        });
        const process_fault::Scope faults(plan);
        result = steamfix::process::monitor(launcher.get(), launcher.pid(), directory.path(), log);
        completed_at = std::chrono::steady_clock::now();
        children_exited_when_monitor_returned =
            ::WaitForSingleObject(launcher.get(), 0) == WAIT_OBJECT_0 &&
            ::WaitForSingleObject(game.get(), 0) == WAIT_OBJECT_0;
        finisher.join();
    }
    require(worker_ok.load(), "test-owned children were not observed and finished within their bounds");
    require(plan.real_candidate_opens > 0, "monitor never queried the actual owned game process");
    require(children_exited_when_monitor_returned,
            "monitor released protection while an owned process remained alive");
    const std::string log = read_log(log_path);
    require_contains(log, "observed process start: destiny2.exe");
    require_contains(log, "observed process exit: destiny2.exe");
    require_contains(log, "both Destiny process categories closed");
    if (unavailable_exit_status) {
        require(plan.exit_status_failures == 1 && plan.exit_status_remaining == 0,
                "the finite exit-status fault did not fire");
        require(!result, "unavailable launcher status was invented");
        require_contains(log, "original launcher exited; exit status is unavailable");
    } else {
        require(result == static_cast<int>(launcher_status), "recovered launcher status changed");
        require(plan.snapshot_creation_failures == 2 && plan.first_entry_failures == 1 &&
                plan.next_entry_failures == 1 && plan.child_wait_failures == 1 &&
                plan.candidate_open_failures == 1,
                "one or more finite observation faults did not fire");
        require(plan.snapshot_creation_remaining == 0 && plan.first_entry_remaining == 0 &&
                plan.next_entry_remaining == 0 && plan.child_wait_remaining == 0 &&
                plan.candidate_open_remaining == 0, "an observation fault remained permanently armed");
        require(plan.post_exit_failure_fired, "post-exit observation failure did not fire");
        require(plan.post_failure_absent_snapshots >= 2,
                "unreliable observation reused a negative poll from before the failure");
        require(completed_at - plan.post_exit_failure_at >= std::chrono::milliseconds(800),
                "unreliable post-exit observation did not reset two-poll release debounce");
        require_contains(log, "original launcher handle unavailable for 1 consecutive polls");
        require_contains(log, "WaitForSingleObject failed");
        require_contains(log, "original launcher handle became queryable again");
        require_contains(log, "process observation unavailable for 1 consecutive polls");
        require_contains(log, "a named Destiny process candidate could not be verified for 1 consecutive polls");
        require_contains(log, "named Destiny process candidates are verifiable again");
    }
}

} // namespace

int main() {
    try {
        run_scenario(false);
        run_scenario(true);
        std::cout << "Real process monitoring recovered from finite observation faults.\n";
    } catch (const std::exception& error) {
        std::cerr << "Process fault regression failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
