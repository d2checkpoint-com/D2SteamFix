#include "steamfix/process.hpp"

#include "steamfix/logging.hpp"
#include "steamfix/paths.hpp"
#include "steamfix/win32.hpp"

#include <tlhelp32.h>

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace steamfix::process {
namespace {

struct ProcessInfo {
    DWORD pid;
    DWORD parent_pid;
    std::wstring name;
};

class ReleaseState {
  public:
    using Clock = std::chrono::steady_clock;

    static constexpr auto launcher_exit_grace = std::chrono::seconds(30);
    static constexpr int exit_debounce_polls = 2;

    [[nodiscard]] bool observe(const bool launcher_alive, const bool game_alive,
                               const bool observation_reliable,
                               const Clock::time_point now) noexcept {
        if (game_alive) game_observed_ = true;
        if (!observation_reliable) {
            reset_negative_observations();
            return false;
        }
        if (launcher_alive || game_alive) {
            reset_negative_observations();
            return false;
        }
        if (!game_observed_) {
            if (!launcher_exit_observed_at_) {
                launcher_exit_observed_at_ = now;
                absent_polls_ = 0;
                return false;
            }
            if (now - *launcher_exit_observed_at_ < launcher_exit_grace) {
                absent_polls_ = 0;
                return false;
            }
        }
        if (absent_polls_ != std::numeric_limits<int>::max()) ++absent_polls_;
        return absent_polls_ >= exit_debounce_polls;
    }

  private:
    void reset_negative_observations() noexcept {
        launcher_exit_observed_at_.reset();
        absent_polls_ = 0;
    }

    std::optional<Clock::time_point> launcher_exit_observed_at_;
    int absent_polls_ = 0;
    bool game_observed_ = false;
};

void increment_saturated(int& value) noexcept {
    if (value != std::numeric_limits<int>::max()) ++value;
}

std::uint64_t creation_key(const HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
}

bool process_name_is(const std::wstring& name, const wchar_t* expected) noexcept {
    return _wcsicmp(name.c_str(), expected) == 0;
}

std::string process_message(const char* event, const std::wstring& name, const DWORD pid) {
    return std::string("observed process ") + event + ": " + narrow(name) +
           ", pid=" + std::to_string(pid);
}

std::vector<ProcessInfo> snapshot() {
    const UniqueHandle handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(handle.get(), &entry)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    std::vector<ProcessInfo> result;
    do {
        result.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile});
    } while (Process32NextW(handle.get(), &entry));
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) {
        throw std::system_error(static_cast<int>(error), std::system_category());
    }
    return result;
}

} // namespace

UniqueHandle open_query(const DWORD pid) {
    return UniqueHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
}

fs::path image_path(const HANDLE process) {
    std::wstring buffer(32768, L'\0');
    DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    buffer.resize(length);
    return fs::path(buffer);
}

std::vector<std::pair<DWORD, std::wstring>> matching_in_dir(const fs::path& directory) {
    std::vector<std::pair<DWORD, std::wstring>> result;
    for (const ProcessInfo& item : snapshot()) {
        if (!process_name_is(item.name, L"destiny2launcher.exe") &&
            !process_name_is(item.name, L"destiny2.exe"))
            continue;
        try {
            const UniqueHandle handle = open_query(item.pid);
            const fs::path path = image_path(handle.get());
            if (!path.has_parent_path()) {
                throw std::runtime_error("queried image path has no containing directory: " +
                                         display_path(path));
            }
            if (paths::equal_windows(path.parent_path(), directory)) {
                result.emplace_back(item.pid, item.name);
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("could not verify existing Destiny process candidate: pid=" +
                                     std::to_string(item.pid) + ", name=" + narrow(item.name) +
                                     ": " + error.what());
        }
    }
    return result;
}

std::optional<int> monitor(const HANDLE child, const DWORD child_pid, const fs::path& game_dir,
                           const Logger& log) noexcept {
    using Key = std::pair<DWORD, std::uint64_t>;
    struct Tracked {
        std::wstring name;
    };
    std::map<Key, Tracked> tracked;
    std::set<Key> previous;
    ReleaseState release;
    int failures = 0;
    bool candidate_uncertain = false;
    int candidate_uncertain_polls = 0;
    int child_wait_failures = 0;
    bool child_finished = false;
    std::optional<int> child_status;

    const auto record_child_wait_failure = [&](std::string detail) {
        increment_saturated(child_wait_failures);
        if (child_wait_failures == 1 || child_wait_failures % 20 == 0) {
            log.warn("original launcher handle unavailable for " +
                     std::to_string(child_wait_failures) +
                     " consecutive polls; retaining renderer protection: " + detail);
        }
    };

    for (;;) {
        try {
            if (!child_finished) {
                const DWORD wait = WaitForSingleObject(child, 0);
                if (wait == WAIT_TIMEOUT) {
                    if (child_wait_failures != 0)
                        log.info("original launcher handle became queryable again");
                    child_wait_failures = 0;
                } else if (wait == WAIT_OBJECT_0) {
                    DWORD status = 0;
                    if (GetExitCodeProcess(child, &status)) {
                        if (child_wait_failures != 0)
                            log.info("original launcher handle became queryable again");
                        child_wait_failures = 0;
                        child_finished = true;
                        child_status = windows_exit_status(status);
                    } else {
                        // The signaled process handle proves the launcher exited.
                        // UAC may provide a waitable handle without exit-code rights.
                        child_finished = true;
                        child_wait_failures = 0;
                        log.warn("original launcher exited; exit status is unavailable: " +
                                 windows_error(GetLastError()));
                    }
                } else {
                    const std::string detail =
                        wait == WAIT_FAILED
                            ? "WaitForSingleObject failed: " + windows_error(GetLastError())
                            : "WaitForSingleObject returned unexpected status " +
                                  std::to_string(wait);
                    record_child_wait_failure(detail);
                }
            }

            try {
                const std::vector<ProcessInfo> items = snapshot();
                std::set<Key> current;
                bool launcher_alive = !child_finished;
                bool game_alive = false;
                bool uncertain = false;
                std::string uncertainty_detail;
                for (const ProcessInfo& item : items) {
                    const bool launcher = process_name_is(item.name, L"destiny2launcher.exe");
                    const bool game = process_name_is(item.name, L"destiny2.exe");
                    if (!launcher && !game) continue;
                    try {
                        const UniqueHandle handle = open_query(item.pid);
                        const fs::path path = image_path(handle.get());
                        if (!path.has_parent_path() ||
                            !paths::equal_windows(path.parent_path(), game_dir))
                            continue;
                        const Key key{item.pid, creation_key(handle.get())};
                        current.insert(key);
                        if (!tracked.contains(key)) tracked.emplace(key, Tracked{item.name});
                        launcher_alive = launcher_alive || launcher;
                        game_alive = game_alive || game;
                    } catch (const std::exception& error) {
                        uncertain = true;
                        uncertainty_detail = "pid=" + std::to_string(item.pid) +
                                             ", name=" + narrow(item.name) + ": " + error.what();
                    }
                }
                for (const Key& key : current) {
                    if (!previous.contains(key))
                        log.info(process_message("start", tracked.at(key).name, key.first));
                }
                for (const Key& key : previous) {
                    if (!current.contains(key)) {
                        const auto found = tracked.find(key);
                        if (found != tracked.end()) {
                            log.info(process_message("exit", found->second.name, key.first));
                            tracked.erase(found);
                        }
                    }
                }
                previous = std::move(current);
                if (!uncertain && candidate_uncertain) {
                    log.info("named Destiny process candidates are verifiable again");
                }
                candidate_uncertain = uncertain;
                if (uncertain) {
                    increment_saturated(candidate_uncertain_polls);
                    if (candidate_uncertain_polls == 1 || candidate_uncertain_polls % 20 == 0) {
                        log.warn("a named Destiny process candidate could not be "
                                 "verified for " +
                                 std::to_string(candidate_uncertain_polls) +
                                 " consecutive polls; retaining renderer protection: " +
                                 uncertainty_detail);
                    }
                } else {
                    candidate_uncertain_polls = 0;
                }
                failures = 0;
                if (release.observe(launcher_alive, game_alive, !uncertain,
                                    ReleaseState::Clock::now()))
                    break;
            } catch (const std::exception& error) {
                increment_saturated(failures);
                if (failures == 1 || failures % 20 == 0) {
                    log.warn("process observation unavailable for " + std::to_string(failures) +
                             " consecutive polls; retaining renderer protection: " + error.what());
                }
                static_cast<void>(release.observe(false, false, false, ReleaseState::Clock::now()));
            }
        } catch (...) {
            increment_saturated(failures);
            log.warn("process-monitor iteration failed unexpectedly; retaining "
                     "renderer protection");
            static_cast<void>(release.observe(false, false, false, ReleaseState::Clock::now()));
        }
        Sleep(500);
    }
    try {
        log.info("both Destiny process categories closed; original launcher pid=" +
                 std::to_string(child_pid));
    } catch (...) {
        log.info("both Destiny process categories closed");
    }
    return child_status;
}

std::vector<DWORD> parent_chain() {
    std::map<DWORD, DWORD> parents;
    for (const ProcessInfo& item : snapshot())
        parents[item.pid] = item.parent_pid;
    DWORD current = GetCurrentProcessId();
    std::vector<DWORD> result;
    for (int depth = 0; depth < 8; ++depth) {
        const auto found = parents.find(current);
        if (found == parents.end() || found->second == 0 || found->second == current) break;
        result.push_back(found->second);
        current = found->second;
    }
    return result;
}

} // namespace steamfix::process
