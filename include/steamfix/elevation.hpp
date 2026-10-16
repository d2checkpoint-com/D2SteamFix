#pragma once

#include "steamfix/win32.hpp"

#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

namespace steamfix::elevation {

class Cancelled final : public std::system_error {
public:
    Cancelled() : std::system_error(ERROR_CANCELLED, std::system_category(),
                                   "Windows permission request was cancelled") {}
};

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
