#include "steamfix/command_line.hpp"
#include "steamfix/ui.hpp"
#include "steamfix/win32.hpp"

#include <shellapi.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace fs = std::filesystem;
using steamfix::UniqueHandle;

constexpr int skipped = 77;
constexpr std::wstring_view information_text = L"D2SteamFix test information: 修復 🎮";
constexpr std::wstring_view error_text = L"D2SteamFix test error: 修復 🎮";

class DesktopUnavailable final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

[[noreturn]] void throw_windows_error() {
    throw std::system_error(steamfix::windows_exit_status(GetLastError()), std::system_category());
}

fs::path current_executable() {
    std::wstring text(32768, L'\0');
    const DWORD written = GetModuleFileNameW(nullptr, text.data(), static_cast<DWORD>(text.size()));
    if (written == 0) throw_windows_error();
    require(written < text.size(), "test executable path exceeded the Windows path limit");
    text.resize(written);
    return fs::path(text);
}

class ChildProcess {
public:
    ChildProcess(const fs::path& executable, const std::wstring_view argument) {
        std::wstring line;
        steamfix::command_line::append_quoted(line, executable.native());
        steamfix::command_line::append_quoted(line, argument);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
                            &startup, &information)) {
            throw_windows_error();
        }
        process_ = UniqueHandle(information.hProcess);
        thread_ = UniqueHandle(information.hThread);
        pid_ = information.dwProcessId;
    }
    ~ChildProcess() noexcept {
        if (process_.get() != nullptr && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
            // This process handle is owned by the immediately preceding
            // CreateProcess call; no enumerated process is ever terminated.
            TerminateProcess(process_.get(), 1);
            WaitForSingleObject(process_.get(), 5000);
        }
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return process_.get(); }
    [[nodiscard]] DWORD pid() const noexcept { return pid_; }
    [[nodiscard]] DWORD exit_status() const {
        DWORD status = 0;
        if (!GetExitCodeProcess(process_.get(), &status)) throw_windows_error();
        return status;
    }
private:
    UniqueHandle process_;
    UniqueHandle thread_;
    DWORD pid_ = 0;
};

bool is_owned_dialog(const HWND window, const DWORD pid) noexcept {
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != pid) return false;
    std::array<wchar_t, 64> class_name{};
    std::array<wchar_t, 128> title{};
    if (GetClassNameW(window, class_name.data(), static_cast<int>(class_name.size())) == 0) return false;
    if (GetWindowTextW(window, title.data(), static_cast<int>(title.size())) == 0) return false;
    return std::wstring_view(class_name.data()) == L"#32770" &&
           std::wstring_view(title.data()) == L"D2SteamFix";
}

struct DialogSearch {
    DWORD pid;
    HWND found = nullptr;
    bool child_window_exists = false;
};

BOOL CALLBACK find_dialog(const HWND window, const LPARAM context) noexcept {
    // Win32 enumeration callbacks carry a borrowed context through LPARAM.
    auto& search = *reinterpret_cast<DialogSearch*>(context);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search.pid) return TRUE;
    search.child_window_exists = true;
    if (!is_owned_dialog(window, search.pid)) return TRUE;
    search.found = window;
    return FALSE;
}

HWND wait_for_dialog(const ChildProcess& child, const DWORD expected_exit, const bool desktop_verified) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool child_window_exists = false;
    while (std::chrono::steady_clock::now() < deadline) {
        DialogSearch search{child.pid()};
        const BOOL enumerated = EnumWindows(find_dialog, reinterpret_cast<LPARAM>(&search));
        if (search.found != nullptr) return search.found;
        child_window_exists = child_window_exists || search.child_window_exists;
        if (!enumerated) {
            if (!desktop_verified) throw DesktopUnavailable("Windows could not enumerate the current desktop");
            throw std::runtime_error("Windows could not enumerate the previously available desktop");
        }
        const DWORD wait = WaitForSingleObject(child.get(), 0);
        if (wait == WAIT_FAILED) throw_windows_error();
        if (wait == WAIT_OBJECT_0) {
            require(child.exit_status() == expected_exit, "dialog child exited with an unexpected status");
            if (!desktop_verified && !child_window_exists) {
                throw DesktopUnavailable("the current desktop could not display the information dialog");
            }
            throw std::runtime_error("child exited without its expected D2SteamFix dialog");
        }
        Sleep(50);
    }
    if (!desktop_verified && !child_window_exists) {
        throw DesktopUnavailable("the current desktop could not expose the child information dialog");
    }
    throw std::runtime_error("child did not display its expected D2SteamFix dialog within eight seconds");
}

struct TextSearch {
    std::wstring_view expected;
    bool found = false;
};

BOOL CALLBACK find_static_text(const HWND control, const LPARAM context) noexcept {
    auto& search = *reinterpret_cast<TextSearch*>(context);
    std::array<wchar_t, 64> class_name{};
    if (GetClassNameW(control, class_name.data(), static_cast<int>(class_name.size())) == 0 ||
        std::wstring_view(class_name.data()) != L"Static") return TRUE;
    if ((GetWindowLongPtrW(control, GWL_STYLE) & SS_TYPEMASK) == SS_ICON) return TRUE;
    std::array<wchar_t, 4096> text{};
    DWORD_PTR length = 0;
    // GetWindowText cannot reliably read controls in another process. WM_GETTEXT
    // is a system message whose buffer Windows marshals across the boundary.
    if (SendMessageTimeoutW(control, WM_GETTEXT, static_cast<WPARAM>(text.size()),
                            reinterpret_cast<LPARAM>(text.data()), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                            500, &length) == 0) return TRUE;
    if (length > 0 && length < text.size() &&
        std::wstring_view(text.data(), static_cast<std::size_t>(length)).find(search.expected) !=
                        std::wstring_view::npos) {
        search.found = true;
        return FALSE;
    }
    return TRUE;
}

void inspect_and_close(const HWND dialog, const ChildProcess& child,
                       const std::wstring_view expected_text, const DWORD expected_exit) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool found = false;
    do {
        require(is_owned_dialog(dialog, child.pid()), "test-owned dialog disappeared before inspection");
        TextSearch text{expected_text};
        EnumChildWindows(dialog, find_static_text, reinterpret_cast<LPARAM>(&text));
        found = text.found;
        if (!found) Sleep(25);
    } while (!found && std::chrono::steady_clock::now() < deadline);
    require(found, "D2SteamFix dialog did not contain its expected message");
    require(is_owned_dialog(dialog, child.pid()), "dialog ownership changed before closing");
    const HWND button = GetDlgItem(dialog, IDOK);
    require(button != nullptr && GetParent(button) == dialog, "owned dialog has no OK button");
    DWORD button_pid = 0;
    GetWindowThreadProcessId(button, &button_pid);
    std::array<wchar_t, 64> button_class{};
    require(button_pid == child.pid() &&
            GetClassNameW(button, button_class.data(), static_cast<int>(button_class.size())) != 0 &&
            std::wstring_view(button_class.data()) == L"Button", "OK button does not belong to the test-owned dialog");
    require(is_owned_dialog(dialog, child.pid()), "dialog ownership changed before its OK button click");
    DWORD_PTR click_result = 0;
    // The native button sends the full WM_COMMAND notification, including its
    // control handle, which MessageBox uses to finish its modal dialog loop.
    const LRESULT clicked = SendMessageTimeoutW(button, BM_CLICK, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &click_result);
    const DWORD wait = WaitForSingleObject(child.get(), 5000);
    if (wait == WAIT_FAILED) throw_windows_error();
    require(clicked != 0 || wait == WAIT_OBJECT_0, "owned dialog OK button did not accept its click");
    require(wait == WAIT_OBJECT_0, "dialog child did not exit after its OK command");
    require(child.exit_status() == expected_exit, "dialog child returned an unexpected exit status");
}

void check_dialog(const fs::path& executable, const std::wstring_view argument,
                   const std::wstring_view expected_text, const DWORD expected_exit,
                   const bool desktop_verified) {
    const ChildProcess child(executable, argument);
    const HWND dialog = wait_for_dialog(child, expected_exit, desktop_verified);
    inspect_and_close(dialog, child, expected_text, expected_exit);
}

struct CommandLineDeleter {
    void operator()(wchar_t** arguments) const noexcept { LocalFree(arguments); }
};

} // namespace

int main() {
    try {
        int count = 0;
        const std::unique_ptr<wchar_t*, CommandLineDeleter> arguments(CommandLineToArgvW(GetCommandLineW(), &count));
        if (!arguments || count != 2) {
            throw std::runtime_error("UI tests require the built steamfix executable path");
        }
        const std::span parsed{arguments.get(), static_cast<std::size_t>(count)};
        const std::wstring_view argument = parsed[1];
        if (argument == L"--show-info") {
            steamfix::ui::info(information_text);
            return 0;
        }
        if (argument == L"--show-error") {
            steamfix::ui::error(error_text);
            return 0;
        }
        const fs::path executable(argument);
        require(fs::is_regular_file(executable), "built steamfix executable is missing");
        const fs::path self = current_executable();
        check_dialog(self, L"--show-info", information_text, 0, false);
        check_dialog(self, L"--show-error", error_text, 0, true);
        check_dialog(executable, L"D2SteamFix-test-invalid-forwarded.exe",
                      L"the forwarded executable must be destiny2launcher.exe", 12, true);
        std::cout << "Real information/error dialogs and steamfix validation entry passed.\n";
    } catch (const DesktopUnavailable& error) {
        std::cout << "SKIP: UI/entry dialog tests: " << error.what() << '\n';
        return skipped;
    } catch (const std::exception& error) {
        std::cerr << "UI/entry integration test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
