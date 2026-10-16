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
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace steamfix::repair {
namespace {

constexpr std::array<const wchar_t*, 2> renderer_names{
    L"GameOverlayRenderer64.dll", L"GameOverlayRenderer.dll"};

[[noreturn]] void throw_win32(const DWORD error) {
    throw std::system_error(static_cast<int>(error), std::system_category());
}

bool missing(const DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

class MutexLock {
public:
    MutexLock(const wchar_t* name, const DWORD timeout) {
        handle_ = UniqueHandle(CreateMutexW(nullptr, FALSE, name));
        const DWORD result = WaitForSingleObject(handle_.get(), timeout);
        if (result == WAIT_TIMEOUT) {
            throw std::runtime_error("another D2SteamFix session did not finish; close it and retry repair");
        }
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) throw_win32(GetLastError());
    }
    ~MutexLock() { ReleaseMutex(handle_.get()); }
    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;
private:
    UniqueHandle handle_;
};

void require_regular(const HANDLE handle, const fs::path& path) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) throw_win32(GetLastError());
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        information.nNumberOfLinks != 1) {
        throw std::runtime_error("repair target is not a regular, single-link, non-reparse file: " + display_path(path));
    }
}

std::optional<UniqueHandle> open_existing(const fs::path& path, const DWORD access) {
    try {
        UniqueHandle handle = elevation::open_file(
            path, access | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT);
        require_regular(handle.get(), path);
        return handle;
    } catch (const std::system_error& error) {
        if (missing(static_cast<DWORD>(error.code().value()))) return std::nullopt;
        throw;
    }
}

UniqueHandle current_token() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw)) {
        throw_win32(GetLastError());
    }
    return UniqueHandle(raw);
}

std::vector<std::byte> token_user(const HANDLE token) {
    DWORD needed = 0;
    if (GetTokenInformation(token, TokenUser, nullptr, 0, &needed) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed < sizeof(TOKEN_USER)) {
        throw std::runtime_error("could not determine Windows user for renderer permissions");
    }
    std::vector<std::byte> result(needed);
    if (!GetTokenInformation(token, TokenUser, result.data(), needed, &needed)) {
        throw_win32(GetLastError());
    }
    return result;
}

bool same_user(const HANDLE first, const HANDLE second) {
    const std::vector<std::byte> a = token_user(first);
    const std::vector<std::byte> b = token_user(second);
    return EqualSid(reinterpret_cast<const TOKEN_USER*>(a.data())->User.Sid,
                    reinterpret_cast<const TOKEN_USER*>(b.data())->User.Sid) != FALSE;
}

bool access_allowed(const HANDLE file, const HANDLE token, const DWORD access) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(file, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) throw_win32(status);
    LocalMemory memory(descriptor);
    HANDLE raw = nullptr;
    if (!DuplicateToken(token, SecurityImpersonation, &raw)) throw_win32(GetLastError());
    const UniqueHandle impersonation(raw);
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    std::vector<std::byte> privileges(sizeof(PRIVILEGE_SET) + 32U * sizeof(LUID_AND_ATTRIBUTES));
    DWORD length = static_cast<DWORD>(privileges.size());
    DWORD granted = 0;
    BOOL allowed = FALSE;
    if (!AccessCheck(descriptor, impersonation.get(), access, &mapping,
                      reinterpret_cast<PPRIVILEGE_SET>(privileges.data()), &length, &granted, &allowed)) {
        throw_win32(GetLastError());
    }
    return allowed != FALSE;
}

bool confirm() {
    const TASKDIALOG_BUTTON buttons[]{{IDOK, L"Continue"}, {IDCANCEL, L"Cancel"}};
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.pszWindowTitle = L"D2SteamFix";
    dialog.pszMainIcon = TD_INFORMATION_ICON;
    dialog.pszMainInstruction = L"Steam and Destiny 2 will close so Steam can repair its files.";
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = IDCANCEL;
    int selected = IDCANCEL;
    const HRESULT result = TaskDialogIndirect(&dialog, &selected, nullptr, nullptr);
    if (FAILED(result)) throw std::runtime_error("could not display Steam repair confirmation");
    return selected == IDOK;
}

fs::path validate_steam(const fs::path& executable) {
    if (!paths::name_is(executable, L"steam.exe")) {
        throw std::runtime_error("select the Steam application's steam.exe file");
    }
    const fs::path directory = fs::canonical(executable.parent_path());
    const auto file = open_existing(directory / L"steam.exe", FILE_READ_ATTRIBUTES);
    if (!file) throw std::runtime_error("steam.exe is missing from the selected directory");
    return directory;
}

std::optional<fs::path> locate_steam(const Logger& log) {
    try {
        return validate_steam(steam::discover() / L"steam.exe");
    } catch (const std::exception& error) {
        log.warn("Steam repair discovery needs a file selection: " + std::string(error.what()));
    }
    for (;;) {
        std::wstring file(32768, L'\0');
        OPENFILENAMEW picker{};
        picker.lStructSize = sizeof(picker);
        picker.lpstrFilter = L"Steam (steam.exe)\0steam.exe\0\0";
        picker.lpstrFile = file.data();
        picker.nMaxFile = static_cast<DWORD>(file.size());
        picker.lpstrTitle = L"Locate Steam's steam.exe";
        picker.lpstrDefExt = L"exe";
        picker.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                       OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
        if (!GetOpenFileNameW(&picker)) {
            const DWORD error = CommDlgExtendedError();
            if (error == 0) return std::nullopt;
            throw std::runtime_error("Steam file selection failed: " + std::to_string(error));
        }
        file.resize(std::wcslen(file.c_str()));
        try {
            return validate_steam(fs::path(file));
        } catch (const std::exception& error) {
            ui::error(L"Select Steam's steam.exe file.\n\n" + widen(error.what()));
        }
    }
}

bool destiny_name(const std::wstring& name) noexcept {
    return _wcsicmp(name.c_str(), L"destiny2.exe") == 0 ||
           _wcsicmp(name.c_str(), L"destiny2launcher.exe") == 0;
}

bool steam_name(const std::wstring& name) noexcept {
    constexpr std::array names{L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe",
        L"GameOverlayUI.exe", L"steamerrorreporter.exe", L"steamerrorreporter64.exe",
        L"steam_monitor.exe"};
    for (const wchar_t* candidate : names) {
        if (_wcsicmp(name.c_str(), candidate) == 0) return true;
    }
    return false;
}

bool inside_steam(const fs::path& image, const fs::path& directory) {
    // Steam's client helpers live in child directories, never in steamapps.
    // Updating Steam may already have renamed or replaced a running image.
    fs::path parent = fs::weakly_canonical(image.parent_path());
    for (unsigned depth = 0; depth < 8; ++depth) {
        if (paths::equal_windows(parent, directory)) return true;
        if (paths::name_is(parent, L"steamapps")) return false;
        const fs::path next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    return false;
}

bool shared_steam_service(const fs::path& image) {
    if (!paths::name_is(image, L"steamservice.exe")) return false;
    for (const int folder : {CSIDL_PROGRAM_FILES_COMMONX86, CSIDL_PROGRAM_FILES_COMMON}) {
        std::array<wchar_t, MAX_PATH> directory{};
        if (FAILED(SHGetFolderPathW(nullptr, folder, nullptr, SHGFP_TYPE_CURRENT, directory.data()))) continue;
        const fs::path expected = fs::path(directory.data()) / L"Steam" / L"SteamService.exe";
        if (paths::equal_windows(image, expected)) return true;
    }
    return false;
}

bool steam_process(const fs::path& image, const fs::path& selected_directory) {
    if (inside_steam(image, selected_directory) || shared_steam_service(image)) return true;
    // Close Steam clients from other installations too, as repair promises to
    // close Steam before replacing files. Verify the installation by its client.
    fs::path parent = fs::weakly_canonical(image.parent_path());
    for (unsigned depth = 0; depth < 8; ++depth) {
        if (paths::name_is(parent, L"steamapps")) return false;
        const DWORD attributes = GetFileAttributesW((parent / L"steam.exe").c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) return true;
        const fs::path next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    return false;
}

struct Candidate {
    DWORD pid;
    std::wstring name;
};

std::vector<Candidate> candidates() {
    const UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W item{};
    item.dwSize = sizeof(item);
    if (!Process32FirstW(snapshot.get(), &item)) throw_win32(GetLastError());
    std::vector<Candidate> result;
    do {
        if (destiny_name(item.szExeFile) || steam_name(item.szExeFile)) {
            result.push_back({item.th32ProcessID, item.szExeFile});
        }
    } while (Process32NextW(snapshot.get(), &item));
    if (GetLastError() != ERROR_NO_MORE_FILES) throw_win32(GetLastError());
    return result;
}

struct Termination {
    UniqueHandle process;
    Candidate candidate;
};

std::vector<Termination> prepare_shutdown(const fs::path& steam_directory) {
    std::vector<Termination> result;
    for (const Candidate& candidate : candidates()) {
        try {
            UniqueHandle handle = elevation::open_process_for_termination(candidate.pid);
            if (WaitForSingleObject(handle.get(), 0) == WAIT_OBJECT_0) continue;
            const fs::path image = process::image_path(handle.get());
            if (!paths::name_is(image, candidate.name)) continue;
            if (!destiny_name(candidate.name) && !steam_process(image, steam_directory)) continue;
            result.push_back({std::move(handle), candidate});
        } catch (const std::system_error& error) {
            if (static_cast<DWORD>(error.code().value()) == ERROR_INVALID_PARAMETER) continue;
            throw;
        }
    }
    return result;
}

void shutdown(const fs::path& steam_directory, const Logger& log) {
    // Prepare every handle before terminating anything, so UAC cancellation is harmless.
    std::vector<Termination> targets = prepare_shutdown(steam_directory);
    for (unsigned pass = 0; pass < 8; ++pass) {
        // Stop the game before Steam, including processes created while Steam was closing.
        for (const bool destiny : {true, false}) {
            for (const Termination& target : targets) {
                if (destiny_name(target.candidate.name) != destiny) continue;
                if (WaitForSingleObject(target.process.get(), 0) == WAIT_OBJECT_0) continue;
                if (!TerminateProcess(target.process.get(), ERROR_CANCELLED)) {
                    const DWORD error = GetLastError();
                    if (WaitForSingleObject(target.process.get(), 0) != WAIT_OBJECT_0) throw_win32(error);
                }
                const DWORD waited = WaitForSingleObject(target.process.get(), 10000);
                if (waited != WAIT_OBJECT_0) {
                    if (waited == WAIT_FAILED) throw_win32(GetLastError());
                    throw std::runtime_error("a Steam or Destiny process did not exit during repair");
                }
                log.info("Steam repair closed " + narrow(target.candidate.name) +
                         "; pid=" + std::to_string(target.candidate.pid));
            }
        }
        targets = prepare_shutdown(steam_directory);
        if (targets.empty()) return;
    }
    throw std::runtime_error("Steam or Destiny processes kept restarting; renderer files were preserved");
}

void remove_file(const fs::path& path, const Logger& log) {
    auto handle = open_existing(path, DELETE);
    if (!handle) return;
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle->get(), FileDispositionInfo,
                                     &disposition, sizeof(disposition))) {
        throw_win32(GetLastError());
    }
    handle.reset();
    log.info("Steam repair removed " + display_path(path));
}

bool remove_renderers(const fs::path& directory, const Logger& log) {
    std::vector<std::pair<fs::path, UniqueHandle>> opened;
    bool renderer32_was_present = false;
    for (const wchar_t* name : renderer_names) {
        const fs::path path = directory / name;
        auto handle = open_existing(path, DELETE);
        if (handle) {
            if (paths::name_is(path, L"GameOverlayRenderer.dll")) renderer32_was_present = true;
            opened.emplace_back(path, std::move(*handle));
        }
    }
    // Acquire and validate both delete handles before removing either renderer.
    for (auto& [path, handle] : opened) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo,
                                        &disposition, sizeof(disposition))) {
            throw_win32(GetLastError());
        }
        handle.reset();
        log.info("Steam repair removed " + display_path(path));
    }
    return renderer32_was_present;
}

UniqueHandle launch_repair(const fs::path& directory, const Logger& log) {
    const fs::path executable = directory / L"steam.exe";
    std::wstring line;
    command_line::append_quoted(line, executable.native());
    line += L" -forcesteamupdate -forcepackagedownload -exitsteam";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    if (!elevation::launch_as_desktop_user(executable, line, directory, information)) {
        if (elevation::is_elevated()) log.warn("desktop launch token unavailable; Steam repair inherits administrator context");
        if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, directory.c_str(), &startup, &information)) {
            throw_win32(GetLastError());
        }
    }
    UniqueHandle child(information.hProcess);
    const UniqueHandle thread(information.hThread);
    log.info("started Steam forced package repair; pid=" + std::to_string(information.dwProcessId));
    return child;
}

bool steam_client_alive(const fs::path& directory) {
    for (const Candidate& candidate : candidates()) {
        if (!steam_name(candidate.name) || _wcsicmp(candidate.name.c_str(), L"steamservice.exe") == 0) continue;
        try {
            const UniqueHandle handle = process::open_query(candidate.pid);
            if (inside_steam(process::image_path(handle.get()), directory)) return true;
        } catch (const std::system_error& error) {
            if (static_cast<DWORD>(error.code().value()) == ERROR_INVALID_PARAMETER) continue;
            throw;
        }
    }
    return false;
}

void wait_for_repair(const HANDLE child, const fs::path& directory, const Logger& log) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::minutes(20);
    std::optional<Clock::time_point> quiet_since;
    for (;;) {
        const DWORD wait = WaitForSingleObject(child, 500);
        if (wait != WAIT_TIMEOUT && wait != WAIT_OBJECT_0) throw_win32(GetLastError());
        if (wait == WAIT_OBJECT_0 && !steam_client_alive(directory)) {
            const auto now = Clock::now();
            if (!quiet_since) quiet_since = now;
            if (now - *quiet_since >= std::chrono::seconds(3)) {
                DWORD code = 0;
                if (!GetExitCodeProcess(child, &code)) throw_win32(GetLastError());
                log.info("Steam repair processes exited; original bootstrap status=" + std::to_string(code));
                return;
            }
        } else {
            quiet_since.reset();
        }
        if (Clock::now() >= deadline) {
            throw std::runtime_error("Steam repair did not exit within 20 minutes; recovery state was preserved");
        }
        if (wait == WAIT_OBJECT_0) Sleep(250);
    }
}

UniqueHandle verification_token(const Logger& log) {
    UniqueHandle caller = current_token();
    if (elevation::is_elevated()) {
        const HWND shell = GetShellWindow();
        DWORD pid = 0;
        if (shell != nullptr) GetWindowThreadProcessId(shell, &pid);
        if (pid != 0) {
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process != nullptr) {
                const UniqueHandle owner(process);
                HANDLE token = nullptr;
                if (OpenProcessToken(owner.get(), TOKEN_QUERY | TOKEN_DUPLICATE, &token)) {
                    UniqueHandle desktop(token);
                    TOKEN_ELEVATION elevation{};
                    DWORD returned = 0;
                    if (GetTokenInformation(desktop.get(), TokenElevation, &elevation,
                                             sizeof(elevation), &returned) &&
                        returned == sizeof(elevation) && elevation.TokenIsElevated == 0 &&
                        same_user(caller.get(), desktop.get())) {
                        return desktop;
                    }
                }
            }
        }
        log.info("same-user normal desktop token unavailable; verifying renderer access in caller context");
    }
    return caller;
}

void verify_repaired(const fs::path& directory, const bool renderer32_was_present,
                     const Logger& log) {
    const UniqueHandle token = verification_token(log);
    for (const wchar_t* name : renderer_names) {
        const fs::path path = directory / name;
        const auto file = open_existing(path, READ_CONTROL);
        if (!file && paths::name_is(path, L"GameOverlayRenderer.dll") && !renderer32_was_present) {
            log.info("optional GameOverlayRenderer.dll was absent before repair and remains absent");
            continue;
        }
        if (!file) throw std::runtime_error("Steam repair did not recreate " + display_path(path));
        if (!access_allowed(file->get(), token.get(), FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)) {
            throw std::runtime_error("Steam recreated a renderer without usable read/execute permissions: " + display_path(path));
        }
        log.info("verified repaired renderer and read/execute access: " + display_path(path));
    }
}

} // namespace

bool needed(const fs::path& steam_directory, const Logger& log) {
    const UniqueHandle token = verification_token(log);
    for (const wchar_t* name : renderer_names) {
        const auto file = open_existing(steam_directory / name, READ_CONTROL);
        if (file && !access_allowed(file->get(), token.get(), FILE_EXECUTE)) return true;
    }
    return false;
}

bool run(const fs::path& game_directory, const fs::path& recovery_path, const Logger& log) {
    try {
        const MutexLock repair_lock(L"Global\\D2SteamFix-Repair-1085660", 0);
        const fs::path game = fs::canonical(game_directory);
        if (!paths::name_is(recovery_path, L"steamfix.dat") ||
            !paths::equal_windows(fs::canonical(recovery_path.parent_path()), game)) {
            throw std::runtime_error("repair recovery path does not match the game directory");
        }
        const std::optional<fs::path> directory = locate_steam(log);
        if (!directory || !confirm()) {
            log.info("Steam repair cancelled; recovery state was preserved");
            return false;
        }
        log.info("Steam repair confirmed; Steam directory=" + display_path(*directory) +
                 "; game directory=" + display_path(game));
        elevation::ensure_broker();
        shutdown(*directory, log);
        // An active wrapper must finish restoring and release its renderer handles first.
        const MutexLock session_lock(L"Global\\D2SteamFix-1085660", 45000);
        // Do not touch renderer files if a new Steam or Destiny process appeared.
        shutdown(*directory, log);
        const bool renderer32_was_present = remove_renderers(*directory, log);
        const UniqueHandle child = launch_repair(*directory, log);
        wait_for_repair(child.get(), *directory, log);
        verify_repaired(*directory, renderer32_was_present, log);
        remove_file(recovery_path, log);
        fs::path temporary = recovery_path;
        temporary += L".tmp";
        remove_file(temporary, log);
        fs::path legacy = recovery_path;
        legacy += L".old";
        remove_file(legacy, log);
        log.info("Steam repair completed; recovery state removed and logs retained");
        ui::info(L"Steam repaired its files. You can start Steam again.");
        return true;
    } catch (const std::system_error& error) {
        if (static_cast<DWORD>(error.code().value()) == ERROR_CANCELLED) {
            log.info("Steam repair elevation declined; recovery state was preserved");
            return false;
        }
        throw AppError(ExitCode::overlay_protection, "Steam file repair", error.what());
    } catch (const AppError&) {
        throw;
    } catch (const std::exception& error) {
        throw AppError(ExitCode::overlay_protection, "Steam file repair", error.what());
    }
}

} // namespace steamfix::repair
