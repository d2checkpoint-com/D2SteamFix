#include "steamfix/elevation.hpp"

#include "steamfix/command_line.hpp"
#include "steamfix/paths.hpp"

#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace steamfix::elevation {
namespace {
constexpr wchar_t helper_switch[] = L"--steamfix-permission-helper";
constexpr wchar_t elevated_launch_switch[] = L"--steamfix-elevated-launch";
std::optional<fs::path> accepted_steam_directory;
constexpr DWORD protocol = 0x31584653;
constexpr DWORD max_path_units = 32767;
constexpr DWORD file_limit = READ_CONTROL | WRITE_DAC | DELETE | FILE_READ_ATTRIBUTES |
                             FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA |
                             FILE_WRITE_ATTRIBUTES | SYNCHRONIZE | GENERIC_READ | GENERIC_WRITE;
constexpr DWORD terminate_access = PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION;
enum class Operation : DWORD { open_file = 1, open_process = 2, move_state = 3 };
struct Request {
    DWORD magic = protocol;
    Operation operation{};
    DWORD access = 0;
    DWORD share = 0;
    DWORD creation = 0;
    DWORD flags = 0;
    DWORD pid = 0;
    DWORD path_units = 0;
    DWORD second_path_units = 0;
};
struct Response {
    DWORD error = ERROR_SUCCESS;
    std::uintptr_t handle = 0;
};

[[noreturn]] void fail(const DWORD error) {
    if (error == ERROR_CANCELLED) throw Cancelled();
    throw std::system_error(static_cast<int>(error), std::system_category());
}

fs::path executable_path(HANDLE process = nullptr) {
    std::wstring value(32768, L'\0');
    DWORD length = static_cast<DWORD>(value.size());
    if (process != nullptr) {
        if (!QueryFullProcessImageNameW(process, 0, value.data(), &length)) fail(GetLastError());
    } else {
        length = GetModuleFileNameW(nullptr, value.data(), length);
        if (length == 0 || length >= value.size()) fail(ERROR_BAD_PATHNAME);
    }
    value.resize(length);
    return fs::path(value);
}

void transfer(const HANDLE pipe, void* data, const size_t size, const bool writing, const DWORD timeout = 60000) {
    auto* bytes = static_cast<std::byte*>(data);
    size_t offset = 0;
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    while (offset < size) {
        const DWORD requested = static_cast<DWORD>(std::min<size_t>(size - offset, MAXDWORD));
        DWORD completed = 0;
        OVERLAPPED overlap{};
        overlap.hEvent = event.get();
        if (!ResetEvent(event.get())) fail(GetLastError());
        const BOOL result = writing ? WriteFile(pipe, bytes + offset, requested, &completed, &overlap)
                                    : ReadFile(pipe, bytes + offset, requested, &completed, &overlap);
        if (!result) {
            if (GetLastError() != ERROR_IO_PENDING) fail(GetLastError());
            const DWORD waited = WaitForSingleObject(event.get(), timeout);
            if (waited != WAIT_OBJECT_0) {
                const DWORD error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                CancelIoEx(pipe, &overlap);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &overlap, &ignored, TRUE);
                fail(error);
            }
            if (!GetOverlappedResult(pipe, &overlap, &completed, FALSE)) fail(GetLastError());
        }
        if (completed == 0) fail(ERROR_BROKEN_PIPE);
        offset += completed;
    }
}

template <typename T>
void write_object(const HANDLE pipe, const T& object) {
    transfer(pipe, const_cast<T*>(&object), sizeof(object), true);
}

template <typename T>
T read_object(const HANDLE pipe, const DWORD timeout = 60000) {
    T result{};
    transfer(pipe, &result, sizeof(result), false, timeout);
    return result;
}

std::vector<std::byte> token_user(const HANDLE process) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) fail(GetLastError());
    const UniqueHandle token(raw);
    DWORD needed = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed);
    if (needed < sizeof(TOKEN_USER)) fail(GetLastError());
    std::vector<std::byte> result(needed);
    if (!GetTokenInformation(token.get(), TokenUser, result.data(), needed, &needed)) fail(GetLastError());
    return result;
}

PSID user_sid(std::vector<std::byte>& user) {
    return reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
}

std::wstring sid_string(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) fail(GetLastError());
    LocalMemory memory(text);
    return text;
}

void enable_privilege(const wchar_t* name) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) {
        fail(GetLastError());
    }
    const UniqueHandle token(raw);
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) {
        fail(GetLastError());
    }
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr) ||
        GetLastError() != ERROR_SUCCESS) fail(GetLastError());
}

class ScopedPrivilege final {
public:
    explicit ScopedPrivilege(const wchar_t* name) {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) fail(GetLastError());
        token_.reset(raw);
        TOKEN_PRIVILEGES requested{};
        requested.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, name, &requested.Privileges[0].Luid)) fail(GetLastError());
        requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD returned = 0;
        SetLastError(ERROR_SUCCESS);
        if (!AdjustTokenPrivileges(token_.get(), FALSE, &requested, sizeof(previous_), &previous_, &returned) ||
            GetLastError() != ERROR_SUCCESS) fail(GetLastError());
    }
    ~ScopedPrivilege() {
        const DWORD saved_error = GetLastError();
        AdjustTokenPrivileges(token_.get(), FALSE, &previous_, 0, nullptr, nullptr);
        SetLastError(saved_error);
    }
    ScopedPrivilege(const ScopedPrivilege&) = delete;
    ScopedPrivilege& operator=(const ScopedPrivilege&) = delete;
private:
    UniqueHandle token_;
    TOKEN_PRIVILEGES previous_{};
};

UniqueHandle process_with_debug_retry(const DWORD access, const DWORD pid) {
    HANDLE raw = OpenProcess(access, FALSE, pid);
    if (raw != nullptr) return UniqueHandle(raw);
    const DWORD error = GetLastError();
    if (!permission_denied(error)) fail(error);
    const ScopedPrivilege privilege(SE_DEBUG_NAME);
    raw = OpenProcess(access, FALSE, pid);
    if (raw == nullptr) fail(GetLastError());
    return UniqueHandle(raw);
}

bool regular(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

void validate_path(const fs::path& path) {
    std::wstring text = path.native();
    // std::filesystem::canonical commonly returns extended drive paths.
    if (text.starts_with(L"\\\\?\\")) text.erase(0, 4);
    if (text.size() < 3 || text[1] != L':' || (text[2] != L'\\' && text[2] != L'/') ||
        !((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) ||
        text.size() > max_path_units || text.find(L'\0') != std::wstring::npos ||
        text.find(L':', 2) != std::wstring::npos) fail(ERROR_BAD_PATHNAME);
    // Do not let a privileged open follow a directory junction or symbolic link.
    fs::path current = fs::path(text).parent_path();
    while (!current.empty()) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) fail(GetLastError());
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) fail(ERROR_REPARSE_TAG_INVALID);
        const fs::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
}

bool renderer(const fs::path& path) {
    return (paths::name_is(path, L"GameOverlayRenderer64.dll") ||
            paths::name_is(path, L"GameOverlayRenderer.dll")) &&
           regular(path.parent_path() / L"steam.exe");
}

bool recovery(const fs::path& path) {
    return (paths::name_is(path, L"steamfix.dat") || paths::name_is(path, L"steamfix.dat.tmp") ||
            paths::name_is(path, L"steamfix.dat.old")) &&
           regular(path.parent_path() / L"destiny2.exe") &&
           regular(path.parent_path() / L"destiny2launcher.exe");
}

void check_owner(const HANDLE file, PSID expected) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                         &owner, nullptr, nullptr, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) fail(result);
    LocalMemory memory(descriptor);
    if (owner == nullptr || !EqualSid(owner, expected)) fail(ERROR_ACCESS_DENIED);
}

void check_regular_handle(const HANDLE file) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) fail(GetLastError());
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        fail(ERROR_REPARSE_TAG_INVALID);
    }
}

UniqueHandle privileged_file(const Request& request, const fs::path& path, PSID originator) {
    validate_path(path);
    if ((request.access & ~file_limit) != 0 ||
        (request.share & ~(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)) != 0 ||
        (request.flags & ~(FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT)) != 0) {
        fail(ERROR_ACCESS_DENIED);
    }
    const bool journal = recovery(path);
    const bool log = paths::name_is(path, L"steamfix.log") &&
                     regular(path.parent_path() / L"destiny2.exe") &&
                     regular(path.parent_path() / L"destiny2launcher.exe");
    if (!journal && !log && !renderer(path)) fail(ERROR_ACCESS_DENIED);
    if (log && (request.creation != OPEN_ALWAYS ||
                (request.access & ~(FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE)) != 0)) {
        fail(ERROR_ACCESS_DENIED);
    }
    if (paths::name_is(path, L"steamfix.dat.old") &&
        (request.creation != OPEN_EXISTING || (request.access & ~(DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES)) != 0)) {
        fail(ERROR_ACCESS_DENIED);
    }
    if (request.creation != OPEN_EXISTING && !(journal && paths::name_is(path, L"steamfix.dat.tmp") &&
                                               request.creation == CREATE_NEW) && !(log && request.creation == OPEN_ALWAYS)) {
        fail(ERROR_ACCESS_DENIED);
    }
    if (!journal && !log && (request.access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA |
                                       FILE_WRITE_ATTRIBUTES)) != 0) fail(ERROR_ACCESS_DENIED);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, FALSE};
    // A credential prompt may use a different administrator account. The backup
    // remains owned/readable by its originating user, who encrypts it with DPAPI.
    if (request.creation == CREATE_NEW || (log && request.creation == OPEN_ALWAYS)) {
        enable_privilege(SE_RESTORE_NAME);
        const std::wstring sid = sid_string(originator);
        const std::wstring sddl = L"O:" + sid + L"D:P(A;;FA;;;" + sid + L")(A;;FA;;;SY)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) fail(GetLastError());
        security.lpSecurityDescriptor = descriptor;
    }
    LocalMemory allocated(descriptor);
    DWORD extra_flags = 0;
    if (journal || log) {
        // A different administrator may approve UAC. Backup semantics let the
        // helper open the originating user's private journal without adding an ACE.
        enable_privilege(SE_BACKUP_NAME);
        enable_privilege(SE_RESTORE_NAME);
        extra_flags = FILE_FLAG_BACKUP_SEMANTICS;
    }
    HANDLE raw = CreateFileW(path.c_str(), request.access | READ_CONTROL, request.share,
                             descriptor == nullptr ? nullptr : &security, request.creation,
                             request.flags | FILE_FLAG_OPEN_REPARSE_POINT | extra_flags, nullptr);
    if (raw == INVALID_HANDLE_VALUE) fail(GetLastError());
    UniqueHandle file(raw);
    check_regular_handle(file.get());
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.get(), &information)) fail(GetLastError());
    if (information.nNumberOfLinks != 1) fail(ERROR_ACCESS_DENIED);
    std::wstring final_path(32768, L'\0');
    const DWORD final_length = GetFinalPathNameByHandleW(file.get(), final_path.data(),
                                                        static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED);
    if (final_length == 0 || final_length >= final_path.size()) fail(ERROR_BAD_PATHNAME);
    final_path.resize(final_length);
    std::wstring expected = path.native();
    if (expected.starts_with(L"\\\\?\\")) expected.erase(0, 4);
    if (final_path.starts_with(L"\\\\?\\")) final_path.erase(0, 4);
    expected = fs::path(expected).lexically_normal().native();
    final_path = fs::path(final_path).lexically_normal().native();
    if (CompareStringOrdinal(expected.c_str(), static_cast<int>(expected.size()), final_path.c_str(),
                             static_cast<int>(final_path.size()), TRUE) != CSTR_EQUAL) fail(ERROR_ACCESS_DENIED);
    const bool cleanup_only = request.creation == OPEN_EXISTING && (request.access & DELETE) != 0 &&
        (request.access & ~(DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES)) == 0;
    if (journal && !cleanup_only) check_owner(file.get(), originator);
    return file;
}

UniqueHandle privileged_process(const DWORD pid) {
    UniqueHandle process = process_with_debug_retry(terminate_access, pid);
    const fs::path image = executable_path(process.get());
    validate_path(image);
    const bool destiny = paths::name_is(image, L"destiny2.exe") ||
                         paths::name_is(image, L"destiny2launcher.exe");
    if (destiny && regular(image.parent_path() / L"destiny2.exe") &&
        regular(image.parent_path() / L"destiny2launcher.exe")) return process;
    constexpr std::array steam_names{L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe",
        L"GameOverlayUI.exe", L"steamerrorreporter.exe", L"steamerrorreporter64.exe", L"steam_monitor.exe"};
    const bool steam = std::any_of(steam_names.begin(), steam_names.end(),
                                  [&](const auto* name) { return paths::name_is(image, name); });
    if (paths::name_is(image, L"steamservice.exe")) {
        constexpr std::array folders{CSIDL_PROGRAM_FILES_COMMONX86, CSIDL_PROGRAM_FILES_COMMON};
        for (const int folder : folders) {
            wchar_t directory[MAX_PATH]{};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, folder, nullptr, SHGFP_TYPE_CURRENT, directory)) &&
                paths::equal_windows(image, fs::path(directory) / L"Steam" / L"SteamService.exe")) return process;
        }
    }
    if (steam) {
        fs::path ancestor = image.parent_path();
        for (unsigned depth = 0; depth < 6 && !ancestor.empty(); ++depth) {
            if (regular(ancestor / L"steam.exe")) return process;
            const fs::path next = ancestor.parent_path();
            if (next == ancestor) break;
            ancestor = next;
        }
    }
    fail(ERROR_ACCESS_DENIED);
}

std::wstring read_path(const HANDLE pipe, const DWORD units) {
    if (units > max_path_units) fail(ERROR_INVALID_DATA);
    std::wstring value(units, L'\0');
    transfer(pipe, value.data(), value.size() * sizeof(wchar_t), false);
    return value;
}

class Broker final {
public:
    void ensure_started() {
        std::lock_guard lock(mutex_);
        if (!pipe_.get()) start();
    }
    Response request(Request request, const fs::path& path = {}, const fs::path& second = {}) {
        std::lock_guard lock(mutex_);
        if (!pipe_.get()) start();
        const std::wstring& text = path.native();
        const std::wstring& second_text = second.native();
        if (text.size() > max_path_units || second_text.size() > max_path_units) fail(ERROR_BAD_PATHNAME);
        request.path_units = static_cast<DWORD>(text.size());
        request.second_path_units = static_cast<DWORD>(second_text.size());
        write_object(pipe_.get(), request);
        transfer(pipe_.get(), const_cast<wchar_t*>(text.data()), text.size() * sizeof(wchar_t), true);
        transfer(pipe_.get(), const_cast<wchar_t*>(second_text.data()), second_text.size() * sizeof(wchar_t), true);
        const Response response = read_object<Response>(pipe_.get());
        if (response.error != ERROR_SUCCESS) fail(response.error);
        return response;
    }
private:
    void start() {
        GUID identifier{};
        if (FAILED(CoCreateGuid(&identifier))) fail(ERROR_GEN_FAILURE);
        wchar_t guid[40]{};
        if (StringFromGUID2(identifier, guid, static_cast<int>(std::size(guid))) == 0) fail(ERROR_GEN_FAILURE);
        const std::wstring pipe_name = L"\\\\.\\pipe\\D2SteamFix-" + std::wstring(guid);
        std::vector<std::byte> user = token_user(GetCurrentProcess());
        const std::wstring sddl = L"D:P(A;;GA;;;" + sid_string(user_sid(user)) +
                                  L")(A;;GA;;;BA)(A;;GA;;;SY)";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) fail(GetLastError());
        LocalMemory memory(descriptor);
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        UniqueHandle listener(CreateNamedPipeW(pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, &security));
        UniqueHandle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED overlap{};
        overlap.hEvent = connected.get();
        const BOOL connecting = ConnectNamedPipe(listener.get(), &overlap);
        const DWORD connection_error = connecting ? ERROR_SUCCESS : GetLastError();
        if (!connecting && connection_error != ERROR_IO_PENDING && connection_error != ERROR_PIPE_CONNECTED) {
            fail(connection_error);
        }
        const bool pending = !connecting && connection_error == ERROR_IO_PENDING;
        if (!pending && !SetEvent(connected.get())) fail(GetLastError());
        // Pending I/O owns overlap until it completes, including cancellation.
        struct CancelConnection {
            HANDLE pipe;
            OVERLAPPED* overlap;
            bool pending;
            ~CancelConnection() {
                if (!pending) return;
                CancelIoEx(pipe, overlap);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, overlap, &ignored, TRUE);
            }
        } connection{listener.get(), &overlap, pending};
        const fs::path executable = executable_path();
        std::wstring parameters;
        command_line::append_quoted(parameters, helper_switch);
        command_line::append_quoted(parameters, std::to_wstring(GetCurrentProcessId()));
        command_line::append_quoted(parameters, pipe_name);
        SHELLEXECUTEINFOW execute{};
        execute.cbSize = sizeof(execute);
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        execute.lpVerb = L"runas";
        execute.lpFile = executable.c_str();
        execute.lpParameters = parameters.c_str();
        execute.nShow = SW_HIDE;
        if (!ShellExecuteExW(&execute)) {
            const DWORD error = GetLastError();
            CancelIoEx(listener.get(), &overlap);
            fail(error);
        }
        UniqueHandle helper(execute.hProcess);
        const std::array waits{connected.get(), helper.get()};
        const DWORD waited = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 60000);
        if (waited != WAIT_OBJECT_0) {
            CancelIoEx(listener.get(), &overlap);
            fail(waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_BROKEN_PIPE);
        }
        DWORD transferred = 0;
        if (pending && !GetOverlappedResult(listener.get(), &overlap, &transferred, FALSE)) fail(GetLastError());
        connection.pending = false;
        ULONG client = 0;
        if (!GetNamedPipeClientProcessId(listener.get(), &client) || client != GetProcessId(helper.get())) {
            fail(ERROR_ACCESS_DENIED);
        }
        pipe_ = std::move(listener);
        helper_ = std::move(helper);
    }
    UniqueHandle pipe_;
    UniqueHandle helper_;
    std::mutex mutex_;
};

Broker& broker() {
    static Broker instance;
    return instance;
}

int helper(const DWORD parent_pid, const std::wstring& pipe_name) {
    if (!is_elevated() || !pipe_name.starts_with(L"\\\\.\\pipe\\D2SteamFix-") ||
        pipe_name.size() > 100 || parent_pid == 0) return ERROR_ACCESS_DENIED;
    constexpr DWORD parent_access = PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
    UniqueHandle parent = process_with_debug_retry(parent_access, parent_pid);
    if (!paths::equal_windows(executable_path(parent.get()), executable_path())) return ERROR_ACCESS_DENIED;
    std::vector<std::byte> user = token_user(parent.get());
    UniqueHandle pipe(CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    ULONG server = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(), &server) || server != parent_pid) return ERROR_ACCESS_DENIED;
    for (;;) {
        Request request{};
        try {
            request = read_object<Request>(pipe.get(), INFINITE);
        } catch (const std::system_error& error) {
            if (error.code().value() == ERROR_BROKEN_PIPE) return 0;
            throw;
        }
        if (request.magic != protocol || request.path_units > max_path_units ||
            request.second_path_units > max_path_units) return ERROR_INVALID_DATA;
        const fs::path path(read_path(pipe.get(), request.path_units));
        const fs::path second(read_path(pipe.get(), request.second_path_units));
        Response response{};
        try {
            UniqueHandle opened;
            if (request.operation == Operation::open_file) {
                opened = privileged_file(request, path, user_sid(user));
            } else if (request.operation == Operation::open_process) {
                opened = privileged_process(request.pid);
            } else if (request.operation == Operation::move_state) {
                validate_path(path);
                validate_path(second);
                if (!recovery(path) || !recovery(second) ||
                    !paths::name_is(path, L"steamfix.dat.tmp") ||
                    !paths::name_is(second, L"steamfix.dat") ||
                    !paths::equal_windows(path.parent_path(), second.parent_path())) fail(ERROR_ACCESS_DENIED);
                Request check{};
                check.access = DELETE | READ_CONTROL;
                check.share = FILE_SHARE_READ | FILE_SHARE_WRITE;
                check.creation = OPEN_EXISTING;
                opened = privileged_file(check, path, user_sid(user));
                check_owner(opened.get(), user_sid(user));
                // Rename the validated source handle. Reopening by pathname
                // would lose the private-journal access granted by backup semantics.
                std::wstring destination = second.native();
                if (destination.starts_with(L"\\\\?\\")) destination.erase(0, 4);
                const size_t name_bytes = destination.size() * sizeof(wchar_t);
                std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + name_bytes);
                auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
                rename->ReplaceIfExists = FALSE;
                rename->RootDirectory = nullptr;
                rename->FileNameLength = static_cast<DWORD>(name_bytes);
                std::memcpy(rename->FileName, destination.data(), name_bytes);
                if (!SetFileInformationByHandle(opened.get(), FileRenameInfo, rename,
                                                static_cast<DWORD>(storage.size()))) fail(GetLastError());
                opened.reset();
            } else {
                fail(ERROR_INVALID_DATA);
            }
            if (opened.get()) {
                HANDLE duplicated = nullptr;
                if (!DuplicateHandle(GetCurrentProcess(), opened.get(), parent.get(), &duplicated,
                                      0, FALSE, DUPLICATE_SAME_ACCESS)) fail(GetLastError());
                response.handle = reinterpret_cast<std::uintptr_t>(duplicated);
            }
        } catch (const std::system_error& error) {
            response.error = static_cast<DWORD>(error.code().value());
        } catch (...) {
            response.error = ERROR_GEN_FAILURE;
        }
        write_object(pipe.get(), response);
    }
}
} // namespace

void ensure_broker() {
    if (!is_elevated()) broker().ensure_started();
}

bool permission_denied(const DWORD error) noexcept {
    return error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD;
}

bool is_elevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) fail(GetLastError());
    const UniqueHandle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &returned)) {
        fail(GetLastError());
    }
    return elevation.TokenIsElevated != 0;
}

UniqueHandle open_file(const fs::path& path, const DWORD access, const DWORD share,
                       const DWORD creation, const DWORD flags, SECURITY_ATTRIBUTES* security) {
    HANDLE raw = CreateFileW(path.c_str(), access, share, security, creation, flags, nullptr);
    if (raw != INVALID_HANDLE_VALUE) return UniqueHandle(raw);
    const DWORD error = GetLastError();
    if (!permission_denied(error)) fail(error);
    Request request{};
    request.operation = Operation::open_file;
    request.access = access;
    request.share = share;
    request.creation = creation;
    request.flags = flags;
    if (is_elevated()) {
        std::vector<std::byte> user = token_user(GetCurrentProcess());
        return privileged_file(request, fs::absolute(path), user_sid(user));
    }
    return UniqueHandle(reinterpret_cast<HANDLE>(broker().request(request, fs::absolute(path)).handle));
}

UniqueHandle open_process_for_termination(const DWORD pid) {
    HANDLE raw = OpenProcess(terminate_access, FALSE, pid);
    if (raw != nullptr) return UniqueHandle(raw);
    const DWORD error = GetLastError();
    if (!permission_denied(error)) fail(error);
    if (is_elevated()) return privileged_process(pid);
    Request request{};
    request.operation = Operation::open_process;
    request.pid = pid;
    return UniqueHandle(reinterpret_cast<HANDLE>(broker().request(request).handle));
}

void move_recovery_file(const fs::path& source, const fs::path& destination) {
    if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) return;
    const DWORD error = GetLastError();
    if (!permission_denied(error) || is_elevated()) fail(error);
    Request request{};
    request.operation = Operation::move_state;
    static_cast<void>(broker().request(request, fs::absolute(source), fs::absolute(destination)));
}

bool remove_recovery_file(const fs::path& path) {
    try {
        UniqueHandle file = open_file(path, DELETE | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                       FILE_SHARE_DELETE, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT);
        check_regular_handle(file.get());
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition,
                                        sizeof(disposition))) fail(GetLastError());
        return true;
    } catch (const std::system_error& error) {
        if (error.code().value() == ERROR_FILE_NOT_FOUND || error.code().value() == ERROR_PATH_NOT_FOUND) return false;
        throw;
    }
}

std::optional<int> dispatch_helper(const std::vector<std::wstring>& arguments) {
    if (arguments.empty() || arguments.front() != helper_switch) return std::nullopt;
    if (arguments.size() != 3) return ERROR_INVALID_PARAMETER;
    try {
        size_t consumed = 0;
        const unsigned long long value = std::stoull(arguments[1], &consumed);
        if (consumed != arguments[1].size() || value > MAXDWORD) return ERROR_INVALID_PARAMETER;
        return helper(static_cast<DWORD>(value), arguments[2]);
    } catch (const std::system_error& error) {
        return error.code().value();
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

void accept_elevated_launch(std::vector<std::wstring>& arguments) {
    if (arguments.empty() || arguments.front() != elevated_launch_switch) return;
    if (arguments.size() < 4 || !is_elevated()) fail(ERROR_INVALID_PARAMETER);
    size_t consumed = 0;
    const unsigned long long value = std::stoull(arguments[1], &consumed);
    if (consumed != arguments[1].size() || value == 0 || value > MAXDWORD ||
        value == GetCurrentProcessId()) fail(ERROR_INVALID_PARAMETER);
    const UniqueHandle parent = process_with_debug_retry(PROCESS_QUERY_LIMITED_INFORMATION,
                                                         static_cast<DWORD>(value));
    if (!paths::equal_windows(executable_path(parent.get()), executable_path())) fail(ERROR_ACCESS_DENIED);
    const fs::path steam_directory = fs::canonical(fs::path(arguments[2]));
    if (!regular(steam_directory / L"steam.exe")) fail(ERROR_FILE_NOT_FOUND);
    // Steam's registry and environment may belong to the original account.
    // Only this authenticated private handoff can supply a discovery override.
    accepted_steam_directory = steam_directory;
    // The complete forwarded command is independently validated by normal entry.
    if (!SetEnvironmentVariableW(L"SteamAppId", L"1085660") ||
        !SetEnvironmentVariableW(L"SteamGameId", L"1085660")) fail(GetLastError());
    arguments.erase(arguments.begin(), arguments.begin() + 3);
}

std::optional<fs::path> inherited_steam_directory() {
    return accepted_steam_directory;
}

std::optional<int> relaunch_as_administrator(const std::vector<std::wstring>& arguments,
                                            const fs::path& directory, const fs::path& steam_directory) {
    if (arguments.empty() || is_elevated()) fail(ERROR_INVALID_PARAMETER);
    const fs::path executable = executable_path();
    std::wstring parameters;
    command_line::append_quoted(parameters, elevated_launch_switch);
    command_line::append_quoted(parameters, std::to_wstring(GetCurrentProcessId()));
    command_line::append_quoted(parameters, steam_directory.native());
    for (const std::wstring& argument : arguments) command_line::append_quoted(parameters, argument);
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = executable.c_str();
    execute.lpParameters = parameters.c_str();
    execute.lpDirectory = directory.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) fail(GetLastError());
    const UniqueHandle child(execute.hProcess);
    const DWORD waited = WaitForSingleObject(child.get(), INFINITE);
    if (waited != WAIT_OBJECT_0) fail(GetLastError());
    DWORD status = 0;
    if (!GetExitCodeProcess(child.get(), &status)) return std::nullopt;
    return static_cast<int>(status);
}

bool launch_as_desktop_user(const fs::path& executable, std::wstring& command_line,
                            const fs::path& directory, PROCESS_INFORMATION& information) {
    if (!is_elevated()) return false;
    const HWND shell = GetShellWindow();
    if (shell == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(shell, &pid);
    if (pid == 0) return false;
    HANDLE process_raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process_raw == nullptr) return false;
    const UniqueHandle process(process_raw);
    HANDLE token_raw = nullptr;
    if (!OpenProcessToken(process.get(), TOKEN_QUERY | TOKEN_DUPLICATE, &token_raw)) return false;
    const UniqueHandle token(token_raw);
    TOKEN_ELEVATION shell_elevation{};
    DWORD returned = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &shell_elevation, sizeof(shell_elevation), &returned) ||
        shell_elevation.TokenIsElevated != 0) return false;
    std::vector<std::byte> caller_user = token_user(GetCurrentProcess());
    std::vector<std::byte> shell_user = token_user(process.get());
    // An explicit Run as different user must keep one coherent deny/DPAPI/game
    // identity. Only drop elevation for a split token of the same Windows user.
    if (!EqualSid(user_sid(caller_user), user_sid(shell_user))) return false;
    HANDLE primary_raw = nullptr;
    if (!DuplicateTokenEx(token.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation,
                           TokenPrimary, &primary_raw)) return false;
    const UniqueHandle primary(primary_raw);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    return CreateProcessWithTokenW(primary.get(), 0, executable.c_str(), command_line.data(), 0,
                                   nullptr, directory.c_str(), &startup, &information) != FALSE;
}

} // namespace steamfix::elevation
