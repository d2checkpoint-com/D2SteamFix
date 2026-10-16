// Exercise the actual private protocol and Win32 helpers without starting UAC.
#include "../src/elevation.cpp"

#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <utility>

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
    require(elevation::rotate_log(log) && !fs::exists(log) &&
            fs::file_size(directory / L"steamfix.log.old.1") > 4U * 1024U * 1024U,
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
        require(parsed != nullptr && count > 0, "Cannot parse the fixture command line");
        std::vector<std::wstring> arguments;
        for (const auto* argument : std::span{parsed.get(), static_cast<std::size_t>(count)}.subspan(1)) {
            arguments.emplace_back(argument);
        }
        if (const auto status = elevation::dispatch_helper(arguments)) return *status;
        check_protocol_and_transport();
        check_paths_and_file_operations();
        check_privilege_restoration();
        check_errors_and_entry_points();
        check_authenticated_helper();
    } catch (const std::exception& error) {
        std::cerr << "Elevation regression test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
