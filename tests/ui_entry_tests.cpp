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
#include <type_traits>
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

enum class DialogFailure { exited_without_dialog, deadline };

class DialogExpectationFailure final : public std::runtime_error {
public:
    explicit DialogExpectationFailure(const DialogFailure reason)
        : std::runtime_error(reason == DialogFailure::exited_without_dialog
                                 ? "child exited without its expected D2SteamFix dialog"
                                 : "child did not display its expected D2SteamFix dialog before the deadline"),
          reason_(reason) {}
    [[nodiscard]] DialogFailure reason() const noexcept { return reason_; }
private:
    DialogFailure reason_;
};

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

[[noreturn]] void throw_windows_error() {
    throw std::system_error(steamfix::windows_exit_status(GetLastError()), std::system_category());
}

[[noreturn]] void desktop_probe_error(const char* operation) {
    const DWORD error = GetLastError();
    if (error == ERROR_ACCESS_DENIED || error == ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION) {
        throw DesktopUnavailable(operation);
    }
    throw std::system_error(steamfix::windows_exit_status(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error),
                            std::system_category(), operation);
}

struct DestroyProbeWindow {
    void operator()(const HWND window) const noexcept { DestroyWindow(window); }
};

struct ProbeSearch {
    HWND expected;
    bool found = false;
};

BOOL CALLBACK find_probe_window(const HWND window, const LPARAM context) noexcept {
    auto& search = *reinterpret_cast<ProbeSearch*>(context);
    if (window == search.expected) search.found = true;
    return TRUE;
}

void verify_desktop() {
    // These desktop/station handles are borrowed; Windows owns their lifetime.
    SetLastError(ERROR_SUCCESS);
    if (GetProcessWindowStation() == nullptr) desktop_probe_error("cannot access the process window station");
    SetLastError(ERROR_SUCCESS);
    if (GetThreadDesktop(GetCurrentThreadId()) == nullptr) desktop_probe_error("cannot access the thread desktop");
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) throw_windows_error();
    SetLastError(ERROR_SUCCESS);
    // This native probe is independent of production dialogs. A hidden top-level
    // window supports enumeration on accessible noninteractive desktops too.
    const std::unique_ptr<std::remove_pointer_t<HWND>, DestroyProbeWindow> probe(
        CreateWindowExW(0, L"STATIC", L"D2SteamFix test desktop probe", WS_POPUP,
                         0, 0, 1, 1, nullptr, nullptr, instance, nullptr));
    if (!probe) desktop_probe_error("cannot create a native desktop probe window");
    ProbeSearch search{probe.get()};
    SetLastError(ERROR_SUCCESS);
    if (!EnumWindows(find_probe_window, reinterpret_cast<LPARAM>(&search))) {
        desktop_probe_error("cannot enumerate the native desktop probe window");
    }
    require(search.found, "desktop enumeration omitted the test-owned native probe window");
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
};

BOOL CALLBACK find_dialog(const HWND window, const LPARAM context) noexcept {
    // Win32 enumeration callbacks carry a borrowed context through LPARAM.
    auto& search = *reinterpret_cast<DialogSearch*>(context);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search.pid) return TRUE;
    if (!is_owned_dialog(window, search.pid)) return TRUE;
    search.found = window;
    return FALSE;
}

HWND wait_for_dialog(const ChildProcess& child, const DWORD expected_exit,
                     const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        DialogSearch search{child.pid()};
        const BOOL enumerated = EnumWindows(find_dialog, reinterpret_cast<LPARAM>(&search));
        if (search.found != nullptr) return search.found;
        if (!enumerated) throw std::runtime_error("Windows could not enumerate the previously verified desktop");
        const DWORD wait = WaitForSingleObject(child.get(), 0);
        if (wait == WAIT_FAILED) throw_windows_error();
        if (wait == WAIT_OBJECT_0) {
            require(child.exit_status() == expected_exit, "dialog child exited with an unexpected status");
            throw DialogExpectationFailure(DialogFailure::exited_without_dialog);
        }
        Sleep(50);
    }
    throw DialogExpectationFailure(DialogFailure::deadline);
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
        // EnumWindows can see MessageBox's dialog before initialization has
        // created its controls and shown it. Wait for the visible modal dialog
        // before sending a close request.
        found = text.found && IsWindowVisible(dialog) && IsWindowEnabled(dialog);
        if (!found) Sleep(25);
    } while (!found && std::chrono::steady_clock::now() < deadline);
    require(found, "D2SteamFix dialog did not contain its expected message");
    require(is_owned_dialog(dialog, child.pid()), "dialog ownership changed before closing");
    DWORD_PTR close_result = 0;
    // An MB_OK message box supports its normal window close action. Sending it
    // directly avoids dependence on native button IDs or foreground focus.
    const LRESULT closed = SendMessageTimeoutW(dialog, WM_CLOSE, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &close_result);
    const DWORD wait = WaitForSingleObject(child.get(), 5000);
    if (wait == WAIT_FAILED) throw_windows_error();
    require(closed != 0 || wait == WAIT_OBJECT_0, "owned dialog did not accept its close request");
    require(wait == WAIT_OBJECT_0, "dialog child did not exit after its close request");
    require(child.exit_status() == expected_exit, "dialog child returned an unexpected exit status");
}

void check_dialog(const fs::path& executable, const std::wstring_view argument,
                   const std::wstring_view expected_text, const DWORD expected_exit) {
    const ChildProcess child(executable, argument);
    const HWND dialog = wait_for_dialog(child, expected_exit, std::chrono::seconds(8));
    inspect_and_close(dialog, child, expected_text, expected_exit);
}

void check_missing_dialog_failure(const fs::path& executable, const std::wstring_view argument,
                                  const DialogFailure expected, const std::chrono::milliseconds timeout) {
    const ChildProcess child(executable, argument);
    try {
        static_cast<void>(wait_for_dialog(child, 0, timeout));
    } catch (const DialogExpectationFailure& error) {
        require(error.reason() == expected, "a no-dialog child failed for the wrong reason");
        return;
    } catch (const DesktopUnavailable&) {
        // A future regression must fail the parent suite rather than reach its
        // desktop-skip handler. Other setup/API errors propagate as failures.
        throw std::runtime_error("a missing or hung dialog was skipped instead of failing");
    }
    throw std::runtime_error("a no-dialog child unexpectedly displayed a D2SteamFix dialog");
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
        if (argument == L"--no-dialog") return 0;
        if (argument == L"--idle-before-dialog") {
            Sleep(30000);
            return 0;
        }
        if (argument == L"--show-info") {
            steamfix::ui::info(information_text);
            return 0;
        }
        if (argument == L"--show-error") {
            steamfix::ui::error(error_text);
            return 0;
        }
        const fs::path self = current_executable();
        const fs::path executable(argument);
        require(fs::is_regular_file(executable), "built steamfix executable is missing");
        verify_desktop();
        check_missing_dialog_failure(self, L"--no-dialog", DialogFailure::exited_without_dialog,
                                     std::chrono::seconds(3));
        check_missing_dialog_failure(self, L"--idle-before-dialog", DialogFailure::deadline,
                                     std::chrono::milliseconds(500));
        check_dialog(self, L"--show-info", information_text, 0);
        check_dialog(self, L"--show-error", error_text, 0);
        check_dialog(executable, L"D2SteamFix-test-invalid-forwarded.exe",
                      L"the forwarded executable must be destiny2launcher.exe", 12);
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
