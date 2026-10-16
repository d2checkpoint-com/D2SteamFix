#pragma once

#include "steamfix/win32.hpp"

#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <utility>
#include <vector>

namespace steamfix::elevation {

class Cancelled final : public std::system_error {
public:
    Cancelled() : std::system_error(ERROR_CANCELLED, std::system_category(),
                                   "Windows permission request was cancelled") {}
};

// Retry the complete coordinator when Windows requires an elevated launcher,
// so backup encryption, deny SID, and the launched game retain one identity.
class RelaunchRequired final : public std::runtime_error {
public:
    RelaunchRequired(DWORD error, std::filesystem::path directory, std::filesystem::path launcher,
                     std::filesystem::path steam_directory)
        : std::runtime_error("launcher requires administrator permission: " + windows_error(error)),
          directory_(std::move(directory)), launcher_(std::move(launcher)),
          steam_directory_(std::move(steam_directory)) {}
    const std::filesystem::path& directory() const noexcept { return directory_; }
    const std::filesystem::path& launcher() const noexcept { return launcher_; }
    const std::filesystem::path& steam_directory() const noexcept { return steam_directory_; }
private:
    std::filesystem::path directory_;
    std::filesystem::path launcher_;
    std::filesystem::path steam_directory_;
};

// Validate and strip the private elevated-coordinator prefix before normal entry.
void accept_elevated_launch(std::vector<std::wstring>& arguments);
std::optional<std::filesystem::path> inherited_steam_directory();
std::optional<int> relaunch_as_administrator(const std::vector<std::wstring>& arguments,
                              const std::filesystem::path& directory,
                              const std::filesystem::path& steam_directory);

bool permission_denied(DWORD error) noexcept;
bool is_elevated();
// Repair may explicitly obtain consent before closing applications.
void ensure_broker();

// Attempt the operation in the caller first. Only permission failures start the
// broker; handles always belong to the caller and are never inheritable.
UniqueHandle open_file(const std::filesystem::path& path, DWORD access, DWORD share,
                       DWORD creation, DWORD flags, SECURITY_ATTRIBUTES* security = nullptr);
UniqueHandle open_process_for_termination(DWORD pid);
void move_recovery_file(const std::filesystem::path& source,
                        const std::filesystem::path& destination);
bool remove_recovery_file(const std::filesystem::path& path);

// The broker dispatch must precede logging, the session mutex, and recovery.
std::optional<int> dispatch_helper(const std::vector<std::wstring>& arguments);

// Use the interactive desktop user's token when an elevated wrapper starts a
// child. Returns false if the desktop token is unavailable or no change is needed.
bool launch_as_desktop_user(const std::filesystem::path& executable,
                            std::wstring& command_line,
                            const std::filesystem::path& directory,
                            PROCESS_INFORMATION& information);

} // namespace steamfix::elevation
