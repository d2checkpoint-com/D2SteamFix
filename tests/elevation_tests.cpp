// Exercise the actual private protocol and Win32 helpers without starting UAC.
#include "steamfix/elevation.hpp"

#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <utility>

namespace elevation_fault {
BOOL WINAPI shell_execute(SHELLEXECUTEINFOW* execute);
BOOL WINAPI token_information(HANDLE token, TOKEN_INFORMATION_CLASS information_class,
                              LPVOID information, DWORD bytes, PDWORD returned);
HANDLE WINAPI create_file(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                          DWORD creation, DWORD flags, HANDLE source);
HANDLE WINAPI open_process(DWORD access, BOOL inherit, DWORD pid);
}

// The shell consent boundary, selected parent's elevation report, and exact
// one-shot permission failures are simulated. Successful I/O, authentication,
// child token reads, handle duplication, and waits use real Windows operations.
#define ShellExecuteExW ::elevation_fault::shell_execute
#define GetTokenInformation ::elevation_fault::token_information
#define CreateFileW ::elevation_fault::create_file
#define OpenProcess ::elevation_fault::open_process
#include "../src/elevation.cpp"
#undef OpenProcess
#undef CreateFileW
#undef GetTokenInformation
#undef ShellExecuteExW

namespace elevation_fault {

constexpr std::wstring_view exit_switch = L"--steamfix-test-shell-exit";
constexpr std::wstring_view relaunch_switch = L"--steamfix-test-handoff";
constexpr DWORD child_status = 37;

void check(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class ChildGuardian final {
public:
    explicit ChildGuardian(const HANDLE process)
        : process_(duplicate(process)), wake_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          watchdog_([this] {
              const std::array waits{process_.get(), wake_.get()};
              if (WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 10000) == WAIT_TIMEOUT) {
                  timed_out_.store(true);
                  TerminateProcess(process_.get(), ERROR_TIMEOUT);
              }
          }) {}
    ~ChildGuardian() noexcept {
        SetEvent(wake_.get());
        if (WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_.get(), ERROR_CANCELLED);
            WaitForSingleObject(process_.get(), 3000);
        }
        // watchdog_ joins before its event and process-handle owners disappear.
    }
    ChildGuardian(const ChildGuardian&) = delete;
    ChildGuardian& operator=(const ChildGuardian&) = delete;
    ChildGuardian(ChildGuardian&&) = delete;
    ChildGuardian& operator=(ChildGuardian&&) = delete;
    DWORD wait() const {
        check(WaitForSingleObject(process_.get(), 10000) == WAIT_OBJECT_0, "The owned shell child did not exit");
        check(!timed_out_.load(), "The owned shell child exceeded its ten-second watchdog");
        DWORD status{};
        if (!GetExitCodeProcess(process_.get(), &status)) steamfix::elevation::fail(GetLastError());
        return status;
    }
    void terminate() const {
        if (!TerminateProcess(process_.get(), ERROR_CANCELLED)) steamfix::elevation::fail(GetLastError());
        check(wait() == ERROR_CANCELLED, "The exclusively owned helper did not terminate as requested");
    }
private:
    static steamfix::UniqueHandle duplicate(const HANDLE process) {
        HANDLE result{};
        if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &result,
                              PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) {
            steamfix::elevation::fail(GetLastError());
        }
        return steamfix::UniqueHandle(result);
    }
    steamfix::UniqueHandle process_;
    steamfix::UniqueHandle wake_;
    std::atomic<bool> timed_out_ = false;
    std::jthread watchdog_;
};

enum class Mode { launch, cancel, exit_before_connection };

struct Plan {
    std::filesystem::path self;
    std::filesystem::path directory;
    bool report_unelevated = false;
    Mode mode = Mode::launch;
    std::size_t shell_calls = 0;
    std::filesystem::path denied_file;
    unsigned file_denials = 0;
    DWORD denied_pid = 0;
    unsigned process_denials = 0;
    std::vector<std::unique_ptr<ChildGuardian>> children;
};

thread_local Plan* active = nullptr;

bool real_elevated() {
    HANDLE raw{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) steamfix::elevation::fail(GetLastError());
    const steamfix::UniqueHandle token(raw);
    TOKEN_ELEVATION information{};
    DWORD returned{};
    if (!::GetTokenInformation(token.get(), TokenElevation, &information, sizeof(information), &returned)) {
        steamfix::elevation::fail(GetLastError());
    }
    return information.TokenIsElevated != 0;
}

class Scope final {
public:
    explicit Scope(Plan& plan) : previous_(active) {
        check(real_elevated(), "The simulated shell boundary requires a real elevated parent token");
        active = &plan;
    }
    ~Scope() noexcept { active = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
private:
    Plan* previous_;
};

BOOL WINAPI token_information(const HANDLE token, const TOKEN_INFORMATION_CLASS information_class,
                              const LPVOID information, const DWORD bytes, const PDWORD returned) {
    const BOOL result = ::GetTokenInformation(token, information_class, information, bytes, returned);
    if (result && active != nullptr && active->report_unelevated && information_class == TokenElevation) {
        check(information != nullptr && bytes >= sizeof(TOKEN_ELEVATION), "Invalid elevation-report fixture buffer");
        auto& elevation = *static_cast<TOKEN_ELEVATION*>(information);
        check(elevation.TokenIsElevated != 0, "A fixture attempted to simulate elevation from an unelevated token");
        elevation.TokenIsElevated = 0;
    }
    return result;
}

HANDLE WINAPI create_file(const LPCWSTR path, const DWORD access, const DWORD share,
                          const LPSECURITY_ATTRIBUTES security, const DWORD creation,
                          const DWORD flags, const HANDLE source) {
    if (active != nullptr && active->file_denials != 0 && path != nullptr &&
        steamfix::paths::equal_windows(std::filesystem::path(path), active->denied_file)) {
        check(active->file_denials == 1 && access == FILE_READ_DATA && share == FILE_SHARE_READ &&
              security == nullptr && creation == OPEN_EXISTING && flags == FILE_ATTRIBUTE_NORMAL && source == nullptr,
              "A file-denial fixture escaped its exact initial renderer request");
        --active->file_denials;
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return ::CreateFileW(path, access, share, security, creation, flags, source);
}

HANDLE WINAPI open_process(const DWORD access, const BOOL inherit, const DWORD pid) {
    if (active != nullptr && active->process_denials != 0 && pid == active->denied_pid) {
        check(active->process_denials == 1 && access == steamfix::elevation::terminate_access && !inherit,
              "A process-denial fixture escaped its exact initial owned-process request");
        --active->process_denials;
        SetLastError(ERROR_PRIVILEGE_NOT_HELD);
        return nullptr;
    }
    return ::OpenProcess(access, inherit, pid);
}

bool validate_shell_request(const SHELLEXECUTEINFOW* execute) {
    check(active != nullptr, "An unscoped shell call was blocked to prevent a UAC prompt");
    check(execute != nullptr && execute->cbSize == sizeof(*execute) && execute->lpVerb != nullptr &&
          std::wstring_view(execute->lpVerb) == L"runas" && execute->lpFile != nullptr &&
          steamfix::paths::equal_windows(std::filesystem::path(execute->lpFile), active->self) &&
          execute->lpParameters != nullptr &&
          execute->fMask == (SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC),
          "The simulated shell request escaped its same-executable boundary");
    std::wstring parsed_command = L"fixture.exe ";
    parsed_command += execute->lpParameters;
    int count{};
    LPWSTR* raw = CommandLineToArgvW(parsed_command.c_str(), &count);
    if (raw == nullptr) steamfix::elevation::fail(GetLastError());
    const steamfix::LocalMemory memory(raw);
    if (count != 4 && count != 7) {
        throw std::runtime_error("The simulated shell request has invalid private arguments");
    }
    const std::span<wchar_t* const> parsed_arguments(raw, static_cast<std::size_t>(count));
    std::vector<std::wstring> arguments;
    arguments.reserve(parsed_arguments.size());
    for (const auto* argument : parsed_arguments) arguments.emplace_back(argument);
    const bool helper = std::wstring_view(arguments[1]) == steamfix::elevation::helper_switch;
    check(std::wstring_view(arguments[2]) == std::to_wstring(GetCurrentProcessId()),
          "The simulated shell request did not authenticate this parent");
    if (helper) {
        const std::wstring_view pipe(arguments[3]);
        check(count == 4 && pipe.starts_with(L"\\\\.\\pipe\\D2SteamFix-") && pipe.size() <= 100 &&
              execute->lpDirectory == nullptr && execute->nShow == SW_HIDE,
              "The simulated helper shell request is not confined to its private pipe");
    } else {
        check(count == 7 && std::wstring_view(arguments[1]) == steamfix::elevation::elevated_launch_switch &&
              steamfix::paths::equal_windows(std::filesystem::path(arguments[3]), active->directory) &&
              std::wstring_view(arguments[4]) == relaunch_switch &&
              std::wstring_view(arguments[5]) == std::to_wstring(child_status) &&
              steamfix::paths::equal_windows(std::filesystem::path(arguments[6]), active->directory) &&
              execute->lpDirectory != nullptr &&
              steamfix::paths::equal_windows(std::filesystem::path(execute->lpDirectory), active->directory) &&
              execute->nShow == SW_SHOWNORMAL,
              "The simulated handoff shell request escaped its fixture directory");
    }
    return helper;
}

BOOL WINAPI shell_execute(SHELLEXECUTEINFOW* execute) {
    const bool helper = validate_shell_request(execute);
    ++active->shell_calls;
    if (active->mode == Mode::cancel) {
        SetLastError(ERROR_CANCELLED);
        return FALSE;
    }
    std::wstring command;
    steamfix::command_line::append_quoted(command, active->self.native());
    if (active->mode == Mode::exit_before_connection) {
        check(helper, "Premature-exit simulation is confined to helper startup");
        steamfix::command_line::append_quoted(command, exit_switch);
    } else {
        command.push_back(L' ');
        command += execute->lpParameters;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(active->self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, execute->lpDirectory, &startup, &information)) return FALSE;
    const steamfix::UniqueHandle thread(information.hThread);
    steamfix::UniqueHandle process(information.hProcess);
    try {
        active->children.push_back(std::make_unique<ChildGuardian>(process.get()));
    } catch (...) {
        TerminateProcess(process.get(), ERROR_CANCELLED);
        WaitForSingleObject(process.get(), 10000);
        throw;
    }
    execute->hProcess = process.release();
    return TRUE;
}

} // namespace elevation_fault

namespace {

namespace elevation = steamfix::elevation;

static_assert(elevation::WireObject<elevation::Request>);
static_assert(elevation::WireObject<elevation::Response>);
static_assert(!elevation::WireObject<std::wstring>);
static_assert(!elevation::WireObject<steamfix::UniqueHandle>);
static_assert(std::is_nothrow_copy_constructible_v<elevation::RelaunchRequired>);
static_assert(std::is_nothrow_copy_assignable_v<elevation::RelaunchRequired>);
static_assert(std::is_nothrow_move_constructible_v<elevation::RelaunchRequired>);
static_assert(std::is_nothrow_move_assignable_v<elevation::RelaunchRequired>);
static_assert(!std::is_copy_constructible_v<elevation::ScopedPrivilege>);
static_assert(!std::is_move_constructible_v<elevation::ScopedPrivilege>);

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <std::invocable Operation>
void expect_error(Operation operation, const DWORD expected) {
    try {
        operation();
    } catch (const std::system_error& error) {
        require(error.code().category() == std::system_category(), "Wrong Windows error category");
        require(error.code().value() == steamfix::windows_exit_status(expected), "Wrong Windows error value");
        return;
    }
    throw std::runtime_error("Expected a Windows error");
}

std::wstring unique_name() {
    GUID identifier{};
    require(SUCCEEDED(CoCreateGuid(&identifier)), "Cannot generate a fixture identifier");
    std::array<wchar_t, 40> text{};
    require(StringFromGUID2(identifier, text.data(), static_cast<int>(text.size())) != 0,
            "Cannot format a fixture identifier");
    return text.data();
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::array<wchar_t, 32768> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        require(length != 0 && length < temporary.size(), "Cannot locate the temporary directory");
        path_ = fs::canonical(fs::path(temporary.data())) / (L"D2SteamFix-elevation-tests-" + unique_name());
        require(fs::create_directory(path_), "Cannot create the temporary fixture directory");
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

void create_file(const fs::path& path, const std::string_view contents = {}) {
    std::ofstream output(path, std::ios::binary);
    output << contents;
    require(output.good(), "Cannot create a fixture file");
}

struct PipePair {
    steamfix::UniqueHandle server;
    steamfix::UniqueHandle client;
};

PipePair create_pipe_pair() {
    const std::wstring name = L"\\\\.\\pipe\\D2SteamFix-tests-" + unique_name();
    steamfix::UniqueHandle server(CreateNamedPipeW(name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr));
    steamfix::UniqueHandle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    steamfix::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED overlap{};
    overlap.hEvent = event.get();
    // Opening the client first avoids a pending connection during construction.
    if (!ConnectNamedPipe(server.get(), &overlap)) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            DWORD ignored{};
            if (!GetOverlappedResult(server.get(), &overlap, &ignored, TRUE)) elevation::fail(GetLastError());
        } else if (error != ERROR_PIPE_CONNECTED) {
            elevation::fail(error);
        }
    }
    return {std::move(server), std::move(client)};
}

class TestChild final {
public:
    TestChild() noexcept = default;
    ~TestChild() noexcept {
        if (process_.get() != nullptr && WaitForSingleObject(process_.get(), 3000) == WAIT_TIMEOUT) {
            // This process was created solely for this fixture. Bound cleanup
            // even when a failed assertion leaves its helper waiting for input.
            TerminateProcess(process_.get(), ERROR_CANCELLED);
            WaitForSingleObject(process_.get(), 3000);
        }
    }
    TestChild(const TestChild&) = delete;
    TestChild& operator=(const TestChild&) = delete;
    TestChild(TestChild&&) = delete;
    TestChild& operator=(TestChild&&) = delete;
    void adopt(const PROCESS_INFORMATION& information) noexcept {
        process_.reset(information.hProcess);
        thread_.reset(information.hThread);
    }
    HANDLE process() const noexcept { return process_.get(); }
    DWORD wait() const {
        const DWORD waited = WaitForSingleObject(process_.get(), 3000);
        if (waited != WAIT_OBJECT_0) {
            elevation::fail(waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        }
        DWORD status{};
        if (!GetExitCodeProcess(process_.get(), &status)) elevation::fail(GetLastError());
        return status;
    }
private:
    steamfix::UniqueHandle process_;
    steamfix::UniqueHandle thread_;
};

class TestConnection final {
public:
    explicit TestConnection(const HANDLE pipe)
        : pipe_(pipe), event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        overlap_.hEvent = event_.get();
        if (!ConnectNamedPipe(pipe_, &overlap_)) {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_PENDING) pending_ = true;
            else if (error != ERROR_PIPE_CONNECTED) elevation::fail(error);
        }
    }
    ~TestConnection() noexcept {
        if (pending_) {
            CancelIoEx(pipe_, &overlap_);
            DWORD ignored{};
            GetOverlappedResult(pipe_, &overlap_, &ignored, TRUE);
        }
    }
    TestConnection(const TestConnection&) = delete;
    TestConnection& operator=(const TestConnection&) = delete;
    TestConnection(TestConnection&&) = delete;
    TestConnection& operator=(TestConnection&&) = delete;
    void wait(const HANDLE child) {
        if (!pending_) return;
        const std::array waits{event_.get(), child};
        // Coverage instrumentation may pause a new child while loading symbols.
        const DWORD waited = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 10000);
        if (waited != WAIT_OBJECT_0) elevation::fail(waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_BROKEN_PIPE);
        DWORD ignored{};
        if (!GetOverlappedResult(pipe_, &overlap_, &ignored, FALSE)) elevation::fail(GetLastError());
        pending_ = false;
    }
private:
    HANDLE pipe_;
    steamfix::UniqueHandle event_;
    OVERLAPPED overlap_{};
    bool pending_ = false;
};

class HelperFixture final {
public:
    HelperFixture() {
        const std::wstring name = L"\\\\.\\pipe\\D2SteamFix-" + unique_name();
        pipe_.reset(CreateNamedPipeW(name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, nullptr));
        if (pipe_.get() == INVALID_HANDLE_VALUE) elevation::fail(GetLastError());
        TestConnection connection(pipe_.get());
        const fs::path executable = elevation::executable_path();
        std::wstring command;
        steamfix::command_line::append_quoted(command, executable.native());
        steamfix::command_line::append_quoted(command, elevation::helper_switch);
        steamfix::command_line::append_quoted(command, std::to_wstring(GetCurrentProcessId()));
        steamfix::command_line::append_quoted(command, name);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information)) elevation::fail(GetLastError());
        child_.adopt(information);
        connection.wait(child_.process());
        ULONG client{};
        require(GetNamedPipeClientProcessId(pipe_.get(), &client) && client == GetProcessId(child_.process()),
                "The fixture pipe connected to an unexpected process");
    }
    ~HelperFixture() noexcept { pipe_.reset(); }
    HelperFixture(const HelperFixture&) = delete;
    HelperFixture& operator=(const HelperFixture&) = delete;
    HelperFixture(HelperFixture&&) = delete;
    HelperFixture& operator=(HelperFixture&&) = delete;
    elevation::Response exchange(elevation::Request request, const fs::path& path = {}, const fs::path& second = {}) {
        require(path.native().size() <= elevation::max_path_units && second.native().size() <= elevation::max_path_units,
                "Fixture paths exceed the helper protocol limit");
        request.path_units = static_cast<DWORD>(path.native().size());
        request.second_path_units = static_cast<DWORD>(second.native().size());
        send(request);
        elevation::transfer(pipe_.get(), std::as_bytes(std::span{path.native()}), 3000);
        elevation::transfer(pipe_.get(), std::as_bytes(std::span{second.native()}), 3000);
        return elevation::read_object<elevation::Response>(pipe_.get(), 3000);
    }
    void send(const elevation::Request& request) {
        elevation::transfer(pipe_.get(), std::as_bytes(std::span{&request, 1}), 3000);
    }
    void send_prefix(const elevation::Request& request) {
        const auto bytes = std::as_bytes(std::span{&request, 1});
        elevation::transfer(pipe_.get(), bytes.first(bytes.size() / 2U), 3000);
    }
    DWORD wait() const { return child_.wait(); }
    DWORD disconnect() {
        pipe_.reset();
        return child_.wait();
    }
private:
    // Destruction closes the pipe before waiting for the exclusively owned child.
    TestChild child_;
    steamfix::UniqueHandle pipe_;
};

void check_protocol_and_transport() {
    PipePair pipe = create_pipe_pair();
    elevation::Request request{};
    request.operation = elevation::Operation::open_file;
    request.access = GENERIC_READ;
    request.share = FILE_SHARE_READ;
    request.creation = OPEN_EXISTING;
    request.flags = FILE_FLAG_OPEN_REPARSE_POINT;
    request.pid = 1234;
    request.path_units = 27;
    request.second_path_units = 12;
    elevation::write_object(pipe.client.get(), request);
    const auto received = elevation::read_object<elevation::Request>(pipe.server.get(), 2000);
    require(std::ranges::equal(std::as_bytes(std::span{&request, 1}),
                               std::as_bytes(std::span{&received, 1})), "Request bytes changed in transit");

    const elevation::Response response{ERROR_ACCESS_DENIED, 0, std::uintptr_t{0x12345678}};
    elevation::write_object(pipe.server.get(), response);
    const auto reply = elevation::read_object<elevation::Response>(pipe.client.get(), 2000);
    require(reply.error == response.error && reply.reserved == 0 && reply.handle == response.handle,
            "Response fields changed in transit");

    const std::wstring path = L"C:\\修復 🎮\\steamfix.dat";
    elevation::transfer(pipe.client.get(), std::as_bytes(std::span{path}), 2000);
    require(elevation::read_path(pipe.server.get(), static_cast<DWORD>(path.size())) == path,
            "The protocol changed a Unicode path");
    require(elevation::read_path(nullptr, 0).empty(), "An empty path was not preserved");
    expect_error([] { static_cast<void>(elevation::read_path(nullptr, elevation::max_path_units + 1)); },
                 ERROR_INVALID_DATA);

    std::vector<std::byte> payload(128U * 1024U);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>(index % 251U);
    }
    std::vector<std::byte> result(payload.size());
    std::exception_ptr reader_error;
    {
        std::jthread reader([&] {
            try {
                elevation::transfer(pipe.server.get(), std::span{result}, 2000);
            } catch (...) {
                reader_error = std::current_exception();
            }
        });
        elevation::transfer(pipe.client.get(), std::as_bytes(std::span{payload}), 2000);
    }
    if (reader_error) std::rethrow_exception(reader_error);
    require(result == payload, "A transfer larger than the pipe buffer lost bytes");

    std::array<std::byte, 1> byte{};
    expect_error([&] { elevation::transfer(pipe.server.get(), std::span{byte}, 10); }, ERROR_TIMEOUT);
    const std::array sent{std::byte{42}};
    elevation::transfer(pipe.client.get(), std::as_bytes(std::span{sent}), 2000);
    elevation::transfer(pipe.server.get(), std::as_writable_bytes(std::span{byte}), 2000);
    require(byte == sent, "Cancelled I/O damaged the next exchange");
    elevation::transfer(nullptr, std::span<std::byte>{}, 10);
    elevation::transfer(nullptr, std::span<const std::byte>{}, 10);

    pipe.client.reset();
    expect_error([&] { elevation::transfer(pipe.server.get(), std::span{byte}, 2000); }, ERROR_BROKEN_PIPE);
}

void check_paths_and_file_operations() {
    const fs::path archive_name(L"C:\\directory\\archive.old");
    const auto rename_bytes = elevation::rename_information(archive_name);
    const std::size_t filename_bytes = archive_name.native().size() * sizeof(wchar_t);
    require(rename_bytes.size() == offsetof(FILE_RENAME_INFO, FileName) + filename_bytes + sizeof(wchar_t),
            "The native rename request has an unexpected byte length");
    require(std::ranges::all_of(std::span{rename_bytes}.first(offsetof(FILE_RENAME_INFO, FileNameLength)),
                               [](const std::byte byte) { return byte == std::byte{}; }),
            "The native rename request contains nonzero replacement/root/reserved bytes");
    FILE_RENAME_INFO native_rename{};
    std::memcpy(&native_rename, rename_bytes.data(), offsetof(FILE_RENAME_INFO, FileName));
    require(native_rename.ReplaceIfExists == FALSE && native_rename.RootDirectory == nullptr &&
            native_rename.FileNameLength == archive_name.native().size() * sizeof(wchar_t),
            "The native rename header does not describe a non-replacing absolute rename");
    require(std::ranges::equal(std::span{rename_bytes}.subspan(offsetof(FILE_RENAME_INFO, FileName), filename_bytes),
                               std::as_bytes(std::span{archive_name.native()})),
            "The native rename filename bytes do not match their declared length");
    require(std::ranges::all_of(std::span{rename_bytes}.last(sizeof(wchar_t)),
                               [](const std::byte byte) { return byte == std::byte{}; }),
            "The native rename filename is missing its trailing Unicode NUL");
    TemporaryDirectory fixture;
    const fs::path& directory = fixture.path();
    elevation::validate_path(directory / L"future.dat");
    for (const std::wstring_view invalid : {L"", L"relative.dat", L"C:relative.dat", L"\\\\server\\share\\file",
                                          L"\\\\.\\device", L"C:\\directory\\file:stream"}) {
        expect_error([&] { elevation::validate_path(fs::path(invalid)); }, ERROR_BAD_PATHNAME);
    }
    std::wstring embedded_null = (directory / L"file").native();
    embedded_null.push_back(L'\0');
    embedded_null += L"ignored";
    expect_error([&] { elevation::validate_path(fs::path(embedded_null)); }, ERROR_BAD_PATHNAME);
    expect_error([] {
        elevation::validate_path(fs::path(L"C:\\" + std::wstring(elevation::max_path_units, L'x')));
    }, ERROR_BAD_PATHNAME);

    require(!elevation::regular(directory) && !elevation::regular(directory / L"missing"),
            "A directory or missing path was classified as a regular file");
    create_file(directory / L"steam.exe");
    create_file(directory / L"destiny2.exe");
    create_file(directory / L"destiny2launcher.exe");
    require(elevation::renderer(directory / L"GameOverlayRenderer64.dll") &&
            elevation::renderer(directory / L"gameoverlayrenderer.dll") &&
            !elevation::renderer(directory / L"other.dll"), "Renderer classification failed");
    require(elevation::recovery(directory / L"steamfix.dat") &&
            elevation::recovery(directory / L"steamfix.dat.tmp") &&
            elevation::recovery(directory / L"steamfix.dat.old") &&
            !elevation::recovery(directory / L"other.dat"), "Recovery path classification failed");
    require(elevation::game_log(directory / L"STEAMFIX.LOG") &&
            !elevation::game_log(directory / L"other.log"), "Log path classification failed");

    const fs::path renderer = directory / L"GameOverlayRenderer64.dll";
    create_file(renderer, "renderer fixture");
    elevation::Request denied{};
    denied.access = WRITE_OWNER;
    expect_error([&] { static_cast<void>(elevation::privileged_file(denied, renderer, nullptr)); }, ERROR_ACCESS_DENIED);
    denied.access = FILE_WRITE_DATA;
    denied.creation = OPEN_EXISTING;
    expect_error([&] { static_cast<void>(elevation::privileged_file(denied, renderer, nullptr)); }, ERROR_ACCESS_DENIED);
    elevation::Request permitted{};
    permitted.access = FILE_READ_DATA;
    permitted.share = FILE_SHARE_READ;
    permitted.creation = OPEN_EXISTING;
    auto opened = elevation::privileged_file(permitted, renderer, nullptr);
    elevation::check_regular_handle(opened.get());
    opened.reset();
    auto normal = elevation::open_file(renderer, FILE_READ_DATA | READ_CONTROL, FILE_SHARE_READ,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    PSID owner{};
    PSECURITY_DESCRIPTOR descriptor{};
    const DWORD owner_result = GetSecurityInfo(normal.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                               &owner, nullptr, nullptr, nullptr, &descriptor);
    if (owner_result != ERROR_SUCCESS) elevation::fail(owner_result);
    const steamfix::LocalMemory owner_memory(descriptor);
    elevation::check_owner(normal.get(), owner);
    std::array<std::byte, SECURITY_MAX_SID_SIZE> world_sid{};
    DWORD sid_bytes = static_cast<DWORD>(world_sid.size());
    require(CreateWellKnownSid(WinWorldSid, nullptr, world_sid.data(), &sid_bytes) != FALSE,
            "Cannot create an owner-mismatch fixture SID");
    expect_error([&] { elevation::check_owner(normal.get(), world_sid.data()); }, ERROR_ACCESS_DENIED);
    normal.reset();
    expect_error([&] {
        static_cast<void>(elevation::checked_file(directory, FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS));
    }, ERROR_REPARSE_TAG_INVALID);

    const fs::path hard_link = directory / L"renderer-hard-link.dll";
    if (CreateHardLinkW(hard_link.c_str(), renderer.c_str(), nullptr)) {
        expect_error([&] {
            static_cast<void>(elevation::checked_file(renderer, FILE_READ_DATA, FILE_SHARE_READ,
                                                     OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT));
        }, ERROR_INVALID_DATA);
    } else {
        std::cout << "SKIP: the temporary filesystem cannot create a hard-link fixture.\n";
    }
    const fs::path directory_link = directory / L"directory-link";
    if (CreateSymbolicLinkW(directory_link.c_str(), directory.c_str(),
                           SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        expect_error([&] { elevation::validate_path(directory_link / L"future.dat"); }, ERROR_REPARSE_TAG_INVALID);
    } else {
        std::cout << "SKIP: this account cannot create a symbolic-link fixture.\n";
    }

    const fs::path source = directory / L"steamfix.dat.tmp";
    const fs::path destination = directory / L"steamfix.dat";
    create_file(source, "private fixture");
    elevation::move_recovery_file(source, destination);
    require(!fs::exists(source) && fs::exists(destination), "Moving a recovery fixture failed");
    require(elevation::remove_recovery_file(destination) && !elevation::remove_recovery_file(destination),
            "Recovery deletion did not distinguish a missing file");
    expect_error([&] { elevation::move_recovery_file(source, destination); }, ERROR_FILE_NOT_FOUND);

    const fs::path log = directory / L"steamfix.log";
    require(!elevation::rotate_log(log), "A missing log was rotated");
    expect_error([&] { static_cast<void>(elevation::rotate_log(directory / L"other.log")); }, ERROR_ACCESS_DENIED);
    create_file(log, "small");
    require(!elevation::rotate_log(log), "A small log was rotated");
    {
        auto file = elevation::open_file(log, GENERIC_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
        LARGE_INTEGER length{};
        length.QuadPart = 4LL * 1024LL * 1024LL + 1;
        require(SetFilePointerEx(file.get(), length, nullptr, FILE_BEGIN) && SetEndOfFile(file.get()),
                "Cannot resize the log fixture");
    }
    create_file(directory / L"steamfix.log.old", "preserve");
    const bool rotated = elevation::rotate_log(log);
    const fs::path expected_archive = directory / L"steamfix.log.old.1";
    if (!rotated || fs::exists(log) || !fs::exists(expected_archive)) {
        std::cerr << "Rotation diagnostics: result=" << rotated
                  << ", native header size=" << sizeof(FILE_RENAME_INFO)
                  << ", replacement width=" << sizeof(native_rename.ReplaceIfExists)
                  << ", root offset=" << offsetof(FILE_RENAME_INFO, RootDirectory)
                  << ", length offset=" << offsetof(FILE_RENAME_INFO, FileNameLength)
                  << ", filename offset=" << offsetof(FILE_RENAME_INFO, FileName) << '\n';
        for (const auto& entry : fs::directory_iterator(directory)) {
            std::error_code error;
            const auto size = entry.file_size(error);
            std::cerr << "Fixture entry: " << steamfix::display_path(entry.path().filename())
                      << ", bytes=" << (error ? 0 : size) << '\n';
        }
        const fs::path first_archive = directory / L"steamfix.log.old";
        if (fs::is_regular_file(first_archive)) {
            std::ifstream contents(first_archive, std::ios::binary);
            std::array<char, 32> prefix{};
            contents.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
            std::cerr << "First archive prefix after rotation: "
                      << std::string_view(prefix.data(), static_cast<std::size_t>(contents.gcount())) << '\n';
        }
        throw std::runtime_error("Rotation did not preserve the occupied archive and create the next archive");
    }
    require(fs::file_size(expected_archive) > 4U * 1024U * 1024U,
            "Oversized log rotation failed");
    std::ifstream archived(directory / L"steamfix.log.old", std::ios::binary);
    const std::string preserved{std::istreambuf_iterator<char>(archived), std::istreambuf_iterator<char>()};
    require(preserved == "preserve", "Rotation replaced an existing archive");
}

std::vector<LUID_AND_ATTRIBUTES> token_privileges() {
    HANDLE raw{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) elevation::fail(GetLastError());
    const steamfix::UniqueHandle token(raw);
    DWORD needed{};
    GetTokenInformation(token.get(), TokenPrivileges, nullptr, 0, &needed);
    require(needed >= offsetof(TOKEN_PRIVILEGES, Privileges), "Invalid token privilege length");
    std::vector<std::byte> storage(needed);
    if (!GetTokenInformation(token.get(), TokenPrivileges, storage.data(), needed, &needed)) elevation::fail(GetLastError());
    DWORD count{};
    std::memcpy(&count, storage.data(), sizeof(count));
    const auto bytes = std::span{storage}.subspan(offsetof(TOKEN_PRIVILEGES, Privileges));
    require(count <= bytes.size() / sizeof(LUID_AND_ATTRIBUTES), "Truncated token privileges");
    std::vector<LUID_AND_ATTRIBUTES> privileges(count);
    std::ranges::copy(bytes.first(privileges.size() * sizeof(LUID_AND_ATTRIBUTES)),
                      std::as_writable_bytes(std::span{privileges}).begin());
    return privileges;
}

void check_privilege_restoration() {
    const auto user = elevation::token_user(GetCurrentProcess());
    require(IsValidSid(elevation::user_sid(user)) && elevation::sid_string(elevation::user_sid(user)).starts_with(L"S-"),
            "Token-user byte marshalling lost the SID");
    expect_error([] { static_cast<void>(elevation::user_sid({})); }, ERROR_INVALID_DATA);
    expect_error([] { const elevation::ScopedPrivilege unknown(L"D2SteamFixUnknownPrivilege"); }, ERROR_NO_SUCH_PRIVILEGE);

    const auto before = token_privileges();
    const auto disabled = std::ranges::find_if(before, [](const auto& privilege) {
        return (privilege.Attributes & (SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_REMOVED)) == 0;
    });
    if (disabled == before.end()) {
        std::cout << "SKIP: token has no disabled privilege for the restoration test.\n";
        return;
    }
    std::array<wchar_t, 256> name{};
    DWORD length = static_cast<DWORD>(name.size());
    LUID identifier = disabled->Luid;
    if (!LookupPrivilegeNameW(nullptr, &identifier, name.data(), &length)) elevation::fail(GetLastError());
    const auto enabled = [&] {
        const auto current = token_privileges();
        const auto found = std::ranges::find_if(current, [&](const auto& privilege) {
            return privilege.Luid.LowPart == identifier.LowPart && privilege.Luid.HighPart == identifier.HighPart;
        });
        require(found != current.end(), "The selected token privilege disappeared");
        return (found->Attributes & SE_PRIVILEGE_ENABLED) != 0;
    };
    {
        const std::lock_guard lock(elevation::privilege_mutex());
        {
            const elevation::ScopedPrivilege privilege(name.data());
            require(enabled(), "The scoped privilege was not enabled");
            SetLastError(ERROR_INVALID_DATA);
        }
        require(GetLastError() == ERROR_INVALID_DATA, "Privilege cleanup replaced the caller's last error");
    }
    require(!enabled(), "The scoped privilege was not restored on normal exit");
    struct TestUnwind final {};
    try {
        const std::lock_guard lock(elevation::privilege_mutex());
        const elevation::ScopedPrivilege privilege(name.data());
        throw TestUnwind{};
    } catch (const TestUnwind&) {
    }
    require(!enabled(), "The scoped privilege was not restored during unwinding");
}

bool has_token_privilege(const wchar_t* name) {
    LUID identifier{};
    if (!LookupPrivilegeValueW(nullptr, name, &identifier)) elevation::fail(GetLastError());
    const auto privileges = token_privileges();
    return std::ranges::any_of(privileges, [&](const auto& privilege) {
        return privilege.Luid.LowPart == identifier.LowPart && privilege.Luid.HighPart == identifier.HighPart &&
               (privilege.Attributes & SE_PRIVILEGE_REMOVED) == 0;
    });
}

void check_authenticated_helper() {
    if (!elevation::is_elevated()) {
        std::cout << "SKIP: authenticated helper-loop fixtures require an already elevated test token; no UAC is started.\n";
        return;
    }
    TemporaryDirectory fixture;
    const fs::path& directory = fixture.path();
    create_file(directory / L"steam.exe");
    create_file(directory / L"destiny2.exe");
    create_file(directory / L"destiny2launcher.exe");
    const fs::path renderer = directory / L"GameOverlayRenderer64.dll";
    constexpr std::string_view renderer_contents = "helper renderer fixture";
    create_file(renderer, renderer_contents);

    HelperFixture helper;
    elevation::Request request{};
    request.operation = elevation::Operation::open_file;
    request.access = WRITE_OWNER;
    request.share = FILE_SHARE_READ;
    request.creation = OPEN_EXISTING;
    auto response = helper.exchange(request, renderer);
    require(response.error == ERROR_ACCESS_DENIED && response.reserved == 0 && response.handle == 0,
            "The helper granted a forbidden file access mask");
    request.access = FILE_READ_DATA;
    request.share = MAXDWORD;
    require(helper.exchange(request, renderer).error == ERROR_ACCESS_DENIED,
            "The helper accepted unsupported sharing flags");
    request.share = FILE_SHARE_READ;
    request.flags = FILE_FLAG_DELETE_ON_CLOSE;
    require(helper.exchange(request, renderer).error == ERROR_ACCESS_DENIED,
            "The helper accepted a destructive file flag");
    request.flags = 0;
    request.creation = CREATE_ALWAYS;
    require(helper.exchange(request, renderer).error == ERROR_ACCESS_DENIED,
            "The helper accepted renderer replacement");
    request.creation = OPEN_EXISTING;
    request.operation = static_cast<elevation::Operation>(MAXDWORD);
    require(helper.exchange(request).error == ERROR_INVALID_DATA, "The helper accepted an unknown operation");
    request.operation = elevation::Operation::open_process;
    request.pid = 0;
    require(helper.exchange(request).error == ERROR_INVALID_PARAMETER,
            "The helper accepted an invalid process identifier");
    request.pid = GetCurrentProcessId();
    response = helper.exchange(request);
    require(response.error != ERROR_SUCCESS && response.handle == 0,
            "The helper granted termination access to the test executable");
    request.operation = elevation::Operation::move_state;
    require(helper.exchange(request, renderer, directory / L"other.dat").error == ERROR_ACCESS_DENIED,
            "The helper accepted an unrelated recovery rename");
    request.operation = elevation::Operation::rotate_log;
    require(helper.exchange(request, directory / L"other.log").error == ERROR_ACCESS_DENIED,
            "The helper accepted an unrelated log path");

    request = elevation::Request{};
    request.operation = elevation::Operation::open_file;
    request.access = FILE_READ_DATA;
    request.share = FILE_SHARE_READ;
    request.creation = OPEN_EXISTING;
    response = helper.exchange(request, renderer);
    require(response.error == ERROR_SUCCESS && response.reserved == 0 && response.handle != 0,
            "The authenticated helper could not open an allowed renderer fixture");
    steamfix::UniqueHandle duplicated(reinterpret_cast<HANDLE>(response.handle));

    if (has_token_privilege(SE_BACKUP_NAME) && has_token_privilege(SE_RESTORE_NAME)) {
        const fs::path temporary = directory / L"steamfix.dat.tmp";
        const fs::path recovery = directory / L"steamfix.dat";
        request.access = GENERIC_WRITE;
        request.share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        request.creation = CREATE_NEW;
        response = helper.exchange(request, temporary);
        require(response.error == ERROR_SUCCESS && response.handle != 0,
                "The helper could not create a private recovery fixture");
        steamfix::UniqueHandle journal(reinterpret_cast<HANDLE>(response.handle));
        constexpr std::string_view journal_contents = "private helper recovery fixture";
        DWORD written{};
        require(WriteFile(journal.get(), journal_contents.data(), static_cast<DWORD>(journal_contents.size()),
                          &written, nullptr) && written == journal_contents.size(),
                "The duplicated recovery handle was not writable");
        const auto user = elevation::token_user(GetCurrentProcess());
        elevation::check_owner(journal.get(), elevation::user_sid(user));
        journal.reset();
        request.operation = elevation::Operation::move_state;
        response = helper.exchange(request, temporary, recovery);
        require(response.error == ERROR_SUCCESS && response.handle == 0 &&
                !fs::exists(temporary) && fs::exists(recovery), "The helper could not rename its private recovery file");

        const fs::path log = directory / L"steamfix.log";
        create_file(log);
        {
            auto file = elevation::open_file(log, GENERIC_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
            LARGE_INTEGER length{};
            length.QuadPart = 4LL * 1024LL * 1024LL + 1;
            require(SetFilePointerEx(file.get(), length, nullptr, FILE_BEGIN) && SetEndOfFile(file.get()),
                    "Cannot resize the helper log fixture");
        }
        create_file(directory / L"steamfix.log.old", "preserve helper archive");
        request.operation = elevation::Operation::rotate_log;
        response = helper.exchange(request, log);
        require(response.error == ERROR_SUCCESS && response.handle == 1 && !fs::exists(log) &&
                fs::file_size(directory / L"steamfix.log.old.1") > 4U * 1024U * 1024U,
                "The helper did not archive its oversized log fixture");
        response = helper.exchange(request, log);
        require(response.error == ERROR_SUCCESS && response.handle == 0,
                "The helper did not defer rotation of a missing log");
    } else {
        std::cout << "SKIP: the elevated token lacks backup/restore privileges for private recovery and log fixtures.\n";
    }

    require(helper.disconnect() == ERROR_SUCCESS, "A helper did not exit cleanly after its pipe disconnected");
    // The transferred handle belongs to this process and remains usable after
    // the helper, including its original source handle, has exited.
    std::array<char, 128> data{};
    DWORD read{};
    require(ReadFile(duplicated.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr) &&
            std::string_view(data.data(), read) == renderer_contents,
            "A duplicated renderer handle did not survive helper exit");
    duplicated.reset();

    {
        HelperFixture malformed;
        elevation::Request invalid{};
        invalid.magic = 0;
        malformed.send(invalid);
        require(malformed.wait() == ERROR_INVALID_DATA, "The helper accepted invalid protocol magic");
    }
    {
        HelperFixture malformed;
        elevation::Request invalid{};
        invalid.path_units = elevation::max_path_units + 1;
        malformed.send(invalid);
        require(malformed.wait() == ERROR_INVALID_DATA, "The helper accepted an oversized path frame");
    }
    {
        HelperFixture truncated;
        const elevation::Request partial{};
        truncated.send_prefix(partial);
        require(truncated.disconnect() == ERROR_SUCCESS, "A truncated request leaked its waiting helper process");
    }
}

void check_broker_and_relaunch_boundaries() {
    if (!elevation_fault::real_elevated()) {
        std::cout << "SKIP: broker/relaunch shell-boundary fixtures require an already elevated token; no UAC is started.\n";
        return;
    }
    TemporaryDirectory fixture;
    const fs::path& directory = fixture.path();
    create_file(directory / L"steam.exe");
    const fs::path renderer = directory / L"GameOverlayRenderer64.dll";
    constexpr std::string_view contents = "broker renderer fixture";
    create_file(renderer, contents);
    const fs::path self = elevation::executable_path();

    elevation_fault::Plan broker_plan;
    broker_plan.self = self;
    broker_plan.directory = directory;
    broker_plan.mode = elevation_fault::Mode::cancel;
    {
        const elevation_fault::Scope boundary(broker_plan);
        {
            elevation::Broker broker;
            expect_error([&] { broker.ensure_started(); }, ERROR_CANCELLED);
            require(broker_plan.shell_calls == 1 && broker_plan.children.empty(),
                    "A cancelled shell request started a child");
            broker_plan.mode = elevation_fault::Mode::launch;
            broker.ensure_started();
            broker.ensure_started();
            require(broker_plan.shell_calls == 2 && broker_plan.children.size() == 1,
                    "A healthy broker did not reuse its authenticated helper");
            elevation::Request request{};
            request.operation = static_cast<elevation::Operation>(MAXDWORD);
            expect_error([&] { static_cast<void>(broker.request(request)); }, ERROR_INVALID_DATA);
            request.operation = elevation::Operation::open_file;
            request.access = FILE_READ_DATA;
            request.share = FILE_SHARE_READ;
            request.creation = OPEN_EXISTING;
            expect_error([&] {
                static_cast<void>(broker.request(request, fs::path(L"C:\\" + std::wstring(elevation::max_path_units, L'x'))));
            }, ERROR_BAD_PATHNAME);
            const auto response = broker.request(request, renderer);
            require(response.handle != 0 && broker_plan.shell_calls == 2,
                    "An application error incorrectly dropped the broker connection");
            steamfix::UniqueHandle transferred(reinterpret_cast<HANDLE>(response.handle));
            std::array<char, 128> bytes{};
            DWORD read{};
            require(ReadFile(transferred.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                    std::string_view(bytes.data(), read) == contents,
                    "A real broker response did not transfer a readable fixture handle");
            transferred.reset();

            broker_plan.children.front()->terminate();
            bool transport_failed = false;
            try {
                static_cast<void>(broker.request(request, renderer));
            } catch (const std::system_error& error) {
                const DWORD code = static_cast<DWORD>(error.code().value());
                require(error.code().category() == std::system_category() &&
                        (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA || code == ERROR_PIPE_NOT_CONNECTED),
                        "The terminated helper did not produce a pipe transport failure");
                transport_failed = true;
            }
            require(transport_failed, "A broker exchange succeeded after its owned helper exited");
            const auto retried = broker.request(request, renderer);
            require(retried.handle != 0 && broker_plan.shell_calls == 3 && broker_plan.children.size() == 2,
                    "A failed transport did not start a fresh authenticated helper");
            const steamfix::UniqueHandle retry_handle(reinterpret_cast<HANDLE>(retried.handle));
        }
        require(broker_plan.children.back()->wait() == ERROR_SUCCESS,
                "Destroying the local broker left its helper waiting on the pipe");
    }

    elevation_fault::Plan early_exit;
    early_exit.self = self;
    early_exit.directory = directory;
    early_exit.mode = elevation_fault::Mode::exit_before_connection;
    {
        const elevation_fault::Scope boundary(early_exit);
        elevation::Broker broker;
        expect_error([&] { broker.ensure_started(); }, ERROR_BROKEN_PIPE);
        require(early_exit.children.size() == 1 && early_exit.children.front()->wait() == elevation_fault::child_status,
                "The broker did not observe its owned helper exit before connecting");
    }

    elevation_fault::Plan relaunch;
    relaunch.self = self;
    relaunch.directory = directory;
    relaunch.report_unelevated = true;
    const std::vector<std::wstring> forwarded{std::wstring(elevation_fault::relaunch_switch),
        std::to_wstring(elevation_fault::child_status), directory.native()};
    {
        const elevation_fault::Scope boundary(relaunch);
        const auto result = elevation::relaunch_as_administrator(forwarded, directory, directory);
        require(result == steamfix::windows_exit_status(elevation_fault::child_status) &&
                relaunch.shell_calls == 1 && relaunch.children.size() == 1 &&
                relaunch.children.front()->wait() == elevation_fault::child_status,
                "The private relaunch did not authenticate its child, forward context, and preserve exit status");
    }
    require(elevation::is_elevated(), "A scoped elevation-report simulation escaped into later tests");
}

void check_elevated_permission_fallbacks() {
    if (!elevation_fault::real_elevated()) {
        std::cout << "SKIP: permission-fallback fixtures require an already elevated token; no UAC is started.\n";
        return;
    }
    TemporaryDirectory fixture;
    const fs::path& directory = fixture.path();
    const fs::path self = elevation::executable_path();
    const fs::path destiny = directory / L"destiny2.exe";
    const fs::path steam = directory / L"steamwebhelper.exe";
    fs::copy_file(self, destiny);
    fs::copy_file(self, steam);
    create_file(directory / L"destiny2launcher.exe");
    create_file(directory / L"steam.exe");
    const fs::path renderer = directory / L"GameOverlayRenderer64.dll";
    constexpr std::string_view contents = "permission fallback renderer";
    create_file(renderer, contents);

    elevation_fault::Plan plan;
    plan.self = self;
    plan.directory = directory;
    const auto start_suspended = [&](const fs::path& image) {
        std::wstring command;
        steamfix::command_line::append_quoted(command, image.native());
        steamfix::command_line::append_quoted(command, elevation_fault::exit_switch);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &information)) {
            elevation::fail(GetLastError());
        }
        const steamfix::UniqueHandle thread(information.hThread);
        steamfix::UniqueHandle process(information.hProcess);
        try {
            plan.children.push_back(std::make_unique<elevation_fault::ChildGuardian>(process.get()));
        } catch (...) {
            TerminateProcess(process.get(), ERROR_CANCELLED);
            WaitForSingleObject(process.get(), 10000);
            throw;
        }
        return process;
    };
    // Copies never execute: suspended children expose only test-owned process
    // images to the production allowlist and remain guarded by retained handles.
    const auto destiny_child = start_suspended(destiny);
    const auto steam_child = start_suspended(steam);
    const DWORD destiny_pid = GetProcessId(destiny_child.get());
    const DWORD steam_pid = GetProcessId(steam_child.get());
    require(destiny_pid != 0 && steam_pid != 0, "Cannot identify the suspended fixture processes");
    {
        const elevation_fault::Scope boundary(plan);
        plan.denied_file = renderer;
        plan.file_denials = 1;
        auto file = elevation::open_file(renderer, FILE_READ_DATA, FILE_SHARE_READ,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
        std::array<char, 128> bytes{};
        DWORD read{};
        require(plan.file_denials == 0 &&
                ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                std::string_view(bytes.data(), read) == contents,
                "The public file opener did not recover from its initial permission denial");

        plan.denied_pid = destiny_pid;
        plan.process_denials = 1;
        auto process = elevation::open_process_for_termination(destiny_pid);
        require(plan.process_denials == 0 && GetProcessId(process.get()) == destiny_pid &&
                WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT && plan.shell_calls == 0,
                "The public process opener did not use its already elevated allowlisted fallback");

        HelperFixture helper;
        elevation::Request request{};
        request.operation = elevation::Operation::open_process;
        request.pid = destiny_pid;
        const auto game_response = helper.exchange(request);
        require(game_response.error == ERROR_SUCCESS && game_response.handle != 0,
                "The authenticated helper rejected its owned game-process fixture");
        const steamfix::UniqueHandle game_handle(reinterpret_cast<HANDLE>(game_response.handle));
        request.pid = steam_pid;
        const auto steam_response = helper.exchange(request);
        require(steam_response.error == ERROR_SUCCESS && steam_response.handle != 0,
                "The authenticated helper rejected its owned Steam-process fixture");
        const steamfix::UniqueHandle steam_handle(reinterpret_cast<HANDLE>(steam_response.handle));
        require(helper.disconnect() == ERROR_SUCCESS && GetProcessId(game_handle.get()) == destiny_pid &&
                GetProcessId(steam_handle.get()) == steam_pid &&
                WaitForSingleObject(game_handle.get(), 0) == WAIT_TIMEOUT &&
                WaitForSingleObject(steam_handle.get(), 0) == WAIT_TIMEOUT,
                "Transferred process handles lost their identity or synchronization rights after helper exit");
        require(TerminateProcess(process.get(), ERROR_CANCELLED) &&
                TerminateProcess(steam_handle.get(), ERROR_CANCELLED) &&
                plan.children[0]->wait() == ERROR_CANCELLED && plan.children[1]->wait() == ERROR_CANCELLED,
                "The public or helper process grant lacks termination rights for its owned fixture");
    }
}

void check_errors_and_entry_points() {
    require(elevation::permission_denied(ERROR_ACCESS_DENIED) &&
            elevation::permission_denied(ERROR_PRIVILEGE_NOT_HELD) &&
            !elevation::permission_denied(ERROR_FILE_NOT_FOUND), "Permission-error classification failed");
    for (const DWORD error : {ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION}) {
        require(elevation::rotation_deferred(error), "A deferred rotation error was not recognized");
    }
    require(!elevation::rotation_deferred(ERROR_ACCESS_DENIED), "A permission error deferred rotation");
    try {
        elevation::fail(ERROR_CANCELLED);
    } catch (const elevation::Cancelled& error) {
        require(error.code().value() == ERROR_CANCELLED, "Cancellation lost its Windows error");
    }
    expect_error([] { elevation::fail(ERROR_FILE_NOT_FOUND); }, ERROR_FILE_NOT_FOUND);
    require(!elevation::dispatch_helper({}) && !elevation::dispatch_helper({L"ordinary"}),
            "Ordinary arguments invoked the private helper");
    require(elevation::dispatch_helper({std::wstring(elevation::helper_switch)}) == ERROR_INVALID_PARAMETER,
            "The helper accepted missing parameters");
    require(elevation::dispatch_helper({std::wstring(elevation::helper_switch), L"4294967296", L"invalid"}) ==
            ERROR_INVALID_PARAMETER, "The helper narrowed an out-of-range process identifier");
    require(elevation::dispatch_helper({std::wstring(elevation::helper_switch), L"1extra", L"invalid"}) ==
            ERROR_INVALID_PARAMETER, "The helper accepted trailing process-identifier text");
    require(elevation::dispatch_helper({std::wstring(elevation::helper_switch), L"not-a-number", L"invalid"}) ==
            ERROR_GEN_FAILURE, "Malformed helper input escaped its exception boundary");
    require(elevation::helper(0, L"invalid") == ERROR_ACCESS_DENIED, "The helper accepted an invalid parent and pipe");
    std::vector<std::wstring> arguments{L"ordinary", L"argument"};
    const auto original = arguments;
    require(!elevation::accept_elevated_launch(arguments) && arguments == original,
            "An ordinary launch changed its arguments or inherited a Steam directory");
    std::vector<std::wstring> missing{std::wstring(elevation::elevated_launch_switch)};
    expect_error([&] { static_cast<void>(elevation::accept_elevated_launch(missing)); }, ERROR_INVALID_PARAMETER);
    std::vector<std::wstring> self{std::wstring(elevation::elevated_launch_switch),
        std::to_wstring(GetCurrentProcessId()), L"unused", L"argument"};
    expect_error([&] { static_cast<void>(elevation::accept_elevated_launch(self)); }, ERROR_INVALID_PARAMETER);
    expect_error([] { static_cast<void>(elevation::relaunch_as_administrator({}, {}, {})); }, ERROR_INVALID_PARAMETER);
    auto process = elevation::open_process_for_termination(GetCurrentProcessId());
    require(GetProcessId(process.get()) == GetCurrentProcessId(), "Opening the current process selected another process");

    elevation::RelaunchRequired original_error(ERROR_ACCESS_DENIED, L"directory", L"launcher", L"steam");
    elevation::RelaunchRequired copied(original_error);
    elevation::RelaunchRequired moved(std::move(original_error));
    copied = std::move(moved);
    for (const auto* error : {&original_error, &copied, &moved}) {
        require(error->directory() == L"directory" && error->launcher() == L"launcher" &&
                error->steam_directory() == L"steam", "Exception copying or moving invalidated its payload");
    }
}

} // namespace

int main() {
    try {
        // The subprocess exercises the production helper entry before running
        // any fixtures; otherwise it would recursively start another test run.
        struct ArgumentsDeleter {
            void operator()(wchar_t** arguments) const noexcept { LocalFree(arguments); }
        };
        int count{};
        const std::unique_ptr<wchar_t*, ArgumentsDeleter> parsed(CommandLineToArgvW(GetCommandLineW(), &count));
        if (!parsed || count <= 0) throw std::runtime_error("Cannot parse the fixture command line");
        const std::span parsed_arguments{parsed.get(), static_cast<std::size_t>(count)};
        std::vector<std::wstring> arguments;
        for (const auto* argument : parsed_arguments.subspan(1)) {
            arguments.emplace_back(argument);
        }
        if (const auto status = elevation::dispatch_helper(arguments)) return *status;
        if (!arguments.empty() && arguments.front() == elevation_fault::exit_switch) {
            require(arguments.size() == 1, "A premature-exit fixture received unexpected arguments");
            return steamfix::windows_exit_status(elevation_fault::child_status);
        }
        if (!arguments.empty() && arguments.front() == elevation::elevated_launch_switch) {
            const auto inherited = elevation::accept_elevated_launch(arguments);
            require(inherited && arguments.size() == 3 && arguments[0] == elevation_fault::relaunch_switch &&
                    arguments[1] == std::to_wstring(elevation_fault::child_status) &&
                    steamfix::paths::equal_windows(*inherited, fs::canonical(fs::path(arguments[2]))),
                    "The elevated handoff changed its authenticated directory or forwarded arguments");
            for (const auto* name : {L"SteamAppId", L"SteamGameId"}) {
                std::array<wchar_t, 16> value{};
                require(GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size())) == 7 &&
                        std::wstring_view(value.data()) == L"1085660",
                        "The elevated handoff did not establish the child's Steam context");
            }
            return steamfix::windows_exit_status(elevation_fault::child_status);
        }
        check_protocol_and_transport();
        check_paths_and_file_operations();
        check_privilege_restoration();
        check_errors_and_entry_points();
        check_authenticated_helper();
        check_broker_and_relaunch_boundaries();
        check_elevated_permission_fallbacks();
    } catch (const std::exception& error) {
        std::cerr << "Elevation regression test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
