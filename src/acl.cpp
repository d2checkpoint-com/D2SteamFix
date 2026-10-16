#include "steamfix/acl.hpp"

#include "steamfix/error.hpp"
#include "steamfix/elevation.hpp"
#include "steamfix/logging.hpp"
#include "steamfix/paths.hpp"
#include "steamfix/win32.hpp"

#include <aclapi.h>
#include <dpapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace fs = std::filesystem;

namespace steamfix::acl {
namespace {

constexpr std::array<std::byte, 8> state_magic{
    std::byte{'S'}, std::byte{'T'}, std::byte{'M'}, std::byte{'F'},
    std::byte{'A'}, std::byte{'C'}, std::byte{'L'}, std::byte{'4'}};
constexpr std::array<std::byte, 8> payload_magic{
    std::byte{'S'}, std::byte{'T'}, std::byte{'M'}, std::byte{'F'},
    std::byte{'D'}, std::byte{'A'}, std::byte{'T'}, std::byte{'4'}};
constexpr std::uintmax_t max_state = 1024U * 1024U;
constexpr std::uintmax_t max_state_file = max_state + 64U * 1024U;
constexpr std::uint32_t max_entries = 2;

static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));

[[noreturn]] void throw_win32(const DWORD code) {
    throw std::system_error(static_cast<int>(code), std::system_category());
}

size_t checked_add(const size_t left, const size_t right) {
    if (right > std::numeric_limits<size_t>::max() - left) {
        throw std::runtime_error("recovery overflow");
    }
    return left + right;
}

void require_range(const std::span<const std::byte> data, const size_t offset,
                   const size_t length, const char* message) {
    if (checked_add(offset, length) > data.size()) throw std::runtime_error(message);
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
T object_at(const std::span<const std::byte> data, const size_t offset, const char* message) {
    require_range(data, offset, sizeof(T), message);
    T result{};
    std::memcpy(&result, data.subspan(offset, sizeof(T)).data(), sizeof(result));
    return result;
}

// These Win32 input parameters predate const-correct interfaces. Only pass the
// resulting pointer to APIs that document the pointed-to storage as input.
void* win32_input(const void* data) noexcept {
    return const_cast<void*>(data);
}

PSID sid_from_bytes(const std::span<const std::byte> data) {
    constexpr size_t sid_header_size = offsetof(SID, SubAuthority);
    require_range(data, 0, sid_header_size, "truncated security descriptor SID");
    const auto revision = std::to_integer<std::uint8_t>(data[0]);
    const auto count = std::to_integer<std::uint8_t>(data[1]);
    if (revision != SID_REVISION || count > SID_MAX_SUB_AUTHORITIES) {
        throw std::runtime_error("invalid security descriptor SID");
    }
    const size_t length = checked_add(sid_header_size, static_cast<size_t>(count) * sizeof(DWORD));
    require_range(data, 0, length, "truncated security descriptor SID");
    const auto sid = win32_input(data.data());
    if (!IsValidSid(sid) || GetLengthSid(sid) != length) {
        throw std::runtime_error("invalid security descriptor SID");
    }
    return sid;
}

void validate_relative_sid(const std::span<const std::byte> data, const DWORD relative_offset) {
    const size_t offset = relative_offset;
    if (offset < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || offset % alignof(DWORD) != 0) {
        throw std::runtime_error("invalid security descriptor SID offset");
    }
    require_range(data, offset, 0, "truncated security descriptor SID");
    static_cast<void>(sid_from_bytes(data.subspan(offset)));
}

void validate_acl(const std::span<const std::byte> data) {
    const ACL acl = object_at<ACL>(data, 0, "truncated security descriptor ACL");
    if (acl.AclSize < sizeof(ACL)) throw std::runtime_error("invalid security descriptor ACL size");
    require_range(data, 0, acl.AclSize, "truncated security descriptor ACL");
    const auto acl_bytes = data.first(acl.AclSize);

    size_t ace_offset = sizeof(ACL);
    for (WORD index = 0; index < acl.AceCount; ++index) {
        const ACE_HEADER header =
            object_at<ACE_HEADER>(acl_bytes, ace_offset, "truncated security descriptor ACE");
        if (header.AceSize < sizeof(ACE_HEADER) || header.AceSize % alignof(DWORD) != 0) {
            throw std::runtime_error("invalid security descriptor ACE size");
        }
        require_range(acl_bytes, ace_offset, header.AceSize, "truncated security descriptor ACE");
        if (header.AceType == ACCESS_DENIED_ACE_TYPE || header.AceType == ACCESS_ALLOWED_ACE_TYPE) {
            constexpr size_t sid_offset = offsetof(ACCESS_DENIED_ACE, SidStart);
            if (header.AceSize < sid_offset) throw std::runtime_error("truncated security descriptor ACE");
            static_cast<void>(sid_from_bytes(
                acl_bytes.subspan(ace_offset + sid_offset, header.AceSize - sid_offset)));
        }
        ace_offset = checked_add(ace_offset, header.AceSize);
    }

    const auto acl_pointer = static_cast<PACL>(win32_input(data.data()));
    if (!IsValidAcl(acl_pointer)) throw std::runtime_error("invalid security descriptor ACL");
}

void validate_relative_acl(const std::span<const std::byte> data, const DWORD relative_offset) {
    const size_t offset = relative_offset;
    if (offset < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || offset % alignof(DWORD) != 0) {
        throw std::runtime_error("invalid security descriptor ACL offset");
    }
    require_range(data, offset, 0, "truncated security descriptor ACL");
    validate_acl(data.subspan(offset));
}

void validate_relative_descriptor(const std::span<const std::byte> bytes) {
    const SECURITY_DESCRIPTOR_RELATIVE descriptor = object_at<SECURITY_DESCRIPTOR_RELATIVE>(
        bytes, 0, "truncated security descriptor");
    if (descriptor.Revision != SECURITY_DESCRIPTOR_REVISION ||
        (descriptor.Control & SE_SELF_RELATIVE) == 0) {
        throw std::runtime_error("invalid self-relative security descriptor");
    }

    if (descriptor.Owner != 0) validate_relative_sid(bytes, descriptor.Owner);
    if (descriptor.Group != 0) validate_relative_sid(bytes, descriptor.Group);
    if ((descriptor.Control & SE_SACL_PRESENT) != 0) {
        if (descriptor.Sacl != 0) validate_relative_acl(bytes, descriptor.Sacl);
    } else if (descriptor.Sacl != 0) {
        throw std::runtime_error("security descriptor has an unexpected SACL offset");
    }
    if ((descriptor.Control & SE_DACL_PRESENT) != 0) {
        if (descriptor.Dacl != 0) validate_relative_acl(bytes, descriptor.Dacl);
    } else if (descriptor.Dacl != 0) {
        throw std::runtime_error("security descriptor has an unexpected DACL offset");
    }

    const auto raw = win32_input(bytes.data());
    if (!IsValidSecurityDescriptor(raw) || GetSecurityDescriptorLength(raw) != bytes.size()) {
        throw std::runtime_error("invalid security descriptor");
    }
}

struct DescriptorParts {
    PACL dacl;
    SECURITY_INFORMATION information;
    SECURITY_DESCRIPTOR_CONTROL control;
};

DescriptorParts descriptor_parts(const std::span<const std::byte> bytes) {
    validate_relative_descriptor(bytes);
    const auto descriptor = win32_input(bytes.data());
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted)) {
        throw_win32(GetLastError());
    }
    if (!present) throw std::runtime_error("security descriptor has no DACL");
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        throw_win32(GetLastError());
    }
    const SECURITY_INFORMATION inheritance =
        (control & SE_DACL_PROTECTED) != 0 ? PROTECTED_DACL_SECURITY_INFORMATION
                                           : UNPROTECTED_DACL_SECURITY_INFORMATION;
    return {dacl, DACL_SECURITY_INFORMATION | inheritance, control};
}

PACL acl_from_bytes(const std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(ACL) || bytes.size() > MAXWORD) {
        throw std::runtime_error("invalid protected ACL size");
    }
    const ACL header = object_at<ACL>(bytes, 0, "truncated protected ACL");
    if (header.AclSize != bytes.size()) throw std::runtime_error("invalid protected ACL length");
    validate_acl(bytes);
    auto* acl = static_cast<PACL>(win32_input(bytes.data()));
    return acl;
}

void validate_utf16(const std::wstring& value) {
    for (size_t index = 0; index < value.size(); ++index) {
        const std::uint16_t unit = static_cast<std::uint16_t>(value[index]);
        if (unit == 0) throw std::runtime_error("recovery path contains an embedded NUL");
        if (unit >= 0xd800U && unit <= 0xdbffU) {
            if (++index >= value.size()) throw std::runtime_error("recovery path has malformed UTF-16");
            const std::uint16_t low = static_cast<std::uint16_t>(value[index]);
            if (low < 0xdc00U || low > 0xdfffU) {
                throw std::runtime_error("recovery path has malformed UTF-16");
            }
        } else if (unit >= 0xdc00U && unit <= 0xdfffU) {
            throw std::runtime_error("recovery path has malformed UTF-16");
        }
    }
}

void validate_target_name(const fs::path& path) {
    const std::wstring native = path.native();
    validate_utf16(native);
    if (!path.is_absolute() || !path.has_parent_path()) {
        throw std::runtime_error("recovery target is not an absolute renderer path");
    }
    if (!paths::name_is(path, L"GameOverlayRenderer64.dll") &&
        !paths::name_is(path, L"GameOverlayRenderer.dll")) {
        throw std::runtime_error("recovery target is not a Steam renderer");
    }
}

bool missing_error(const DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool has_regular_steam_neighbor(const fs::path& path) {
    const fs::path steam = path.parent_path() / L"steam.exe";
    const DWORD attributes = GetFileAttributesW(steam.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (missing_error(error)) return false;
        throw_win32(error);
    }
    return (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

void require_regular_steam_neighbor(const fs::path& path) {
    if (!has_regular_steam_neighbor(path)) {
        throw std::runtime_error("recovery target is not beside a regular, non-reparse steam.exe");
    }
}

struct FileIdentity {
    std::uint32_t volume_serial;
    std::uint64_t file_index;

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

class UnsafeTarget final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct OpenedTarget {
    UniqueHandle handle;
    FileIdentity identity;
};

OpenedTarget open_target(const fs::path& path, const DWORD desired_access) {
    validate_target_name(path);
    UniqueHandle handle = elevation::open_file(path, desired_access,
        FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle.get(), &information)) throw_win32(GetLastError());
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        throw UnsafeTarget("renderer target is not a regular, non-reparse file");
    }
    const std::uint64_t index =
        (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
        information.nFileIndexLow;
    return {std::move(handle), {information.dwVolumeSerialNumber, index}};
}

struct OpenBackup {
    Backup backup;
    UniqueHandle handle;
    FileIdentity identity;
};

OpenBackup make_backup(const fs::path& path) {
    validate_target_name(path);
    require_regular_steam_neighbor(path);
    OpenedTarget opened = open_target(path, READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(opened.handle.get(), SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                         nullptr, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) throw_win32(status);
    LocalMemory memory(descriptor);
    const DWORD length = GetSecurityDescriptorLength(descriptor);
    if (length == 0 || length > max_state) {
        throw std::runtime_error("invalid security descriptor length");
    }
    Backup backup{path, std::vector<std::byte>(length), {}};
    std::memcpy(backup.descriptor.data(), descriptor, length);
    static_cast<void>(descriptor_parts(backup.descriptor));
    return {std::move(backup), std::move(opened.handle), opened.identity};
}

class UserIdentity final {
public:
    explicit UserIdentity(const std::span<const std::byte> sid)
        : data_(sid.begin(), sid.end()) {
        static_cast<void>(sid_from_bytes(data_));
    }

    [[nodiscard]] PSID sid() noexcept { return data_.data(); }

private:
    std::vector<std::byte> data_;
};

UserIdentity current_user() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw_win32(GetLastError());
    UniqueHandle token(raw);
    DWORD needed = 0;
    SetLastError(ERROR_SUCCESS);
    static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed));
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed < sizeof(TOKEN_USER)) {
        const DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) throw std::runtime_error("invalid current-user token size");
        throw_win32(error);
    }
    std::vector<std::byte> data(needed);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), needed, &needed)) {
        throw_win32(GetLastError());
    }
    const TOKEN_USER user = object_at<TOKEN_USER>(data, 0, "truncated current-user token");
    if (!IsValidSid(user.User.Sid)) throw std::runtime_error("invalid current-user SID");
    return UserIdentity(std::span(static_cast<const std::byte*>(user.User.Sid),
                                  GetLengthSid(user.User.Sid)));
}

class RestrictedSecurity final {
public:
    ~RestrictedSecurity() = default;
    RestrictedSecurity(const RestrictedSecurity&) = delete;
    RestrictedSecurity& operator=(const RestrictedSecurity&) = delete;
    RestrictedSecurity(RestrictedSecurity&&) = delete;
    RestrictedSecurity& operator=(RestrictedSecurity&&) = delete;

    RestrictedSecurity(PSID user_sid, const DWORD inheritance_flags) {
        DWORD system_sid_size = static_cast<DWORD>(system_sid_.size());
        if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid_.data(),
                                &system_sid_size)) {
            throw_win32(GetLastError());
        }
        const DWORD user_sid_size = GetLengthSid(user_sid);
        if (user_sid_size == 0) throw_win32(GetLastError());
        const size_t ace_base = sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD);
        const size_t acl_size = sizeof(ACL) + ace_base + user_sid_size +
                                ace_base + system_sid_size;
        if (acl_size > MAXWORD) throw std::runtime_error("private ACL is too large");
        acl_data_.resize(acl_size);
        acl_ = reinterpret_cast<PACL>(acl_data_.data());
        if (!InitializeAcl(acl_, static_cast<DWORD>(acl_data_.size()), ACL_REVISION) ||
            !AddAccessAllowedAceEx(acl_, ACL_REVISION, inheritance_flags,
                                   FILE_ALL_ACCESS, user_sid) ||
            !AddAccessAllowedAceEx(acl_, ACL_REVISION, inheritance_flags,
                                   FILE_ALL_ACCESS, system_sid_.data()) ||
            !InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE) ||
            !SetSecurityDescriptorOwner(&descriptor_, user_sid, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED,
                                          SE_DACL_PROTECTED)) {
            throw_win32(GetLastError());
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
    }

    [[nodiscard]] PACL acl() const noexcept { return acl_; }
    [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept { return &attributes_; }

private:
    alignas(DWORD) std::array<std::byte, SECURITY_MAX_SID_SIZE> system_sid_{};
    std::vector<std::byte> acl_data_;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
    PACL acl_ = nullptr;
};

void require_current_user_owner(const HANDLE handle, PSID expected_owner) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                         &owner, nullptr, nullptr, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) throw_win32(status);
    LocalMemory memory(descriptor);
    if (owner == nullptr || !EqualSid(owner, expected_owner)) {
        throw std::runtime_error("recovery storage is not owned by the current Windows user");
    }
}

UniqueHandle probe_access(const fs::path& path, const DWORD access) {
    const HANDLE raw = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return {};
    return UniqueHandle(raw);
}

void verify_denied(const Backup& backup, HANDLE handle);

bool has_current_user_execute_deny(const Backup& backup, PSID sid) {
    const DescriptorParts original = descriptor_parts(backup.descriptor);
    if (original.dacl == nullptr) return false;
    ACL_SIZE_INFORMATION size{};
    if (!GetAclInformation(original.dacl, &size, sizeof(size), AclSizeInformation)) {
        throw_win32(GetLastError());
    }
    for (DWORD index = 0; index < size.AceCount; ++index) {
        void* ace = nullptr;
        if (!GetAce(original.dacl, index, &ace)) throw_win32(GetLastError());
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_DENIED_ACE_TYPE ||
            (header->AceFlags & INHERITED_ACE) != 0) continue;
        const auto* denied = static_cast<const ACCESS_DENIED_ACE*>(ace);
        if ((denied->Mask & FILE_EXECUTE) != 0 &&
            EqualSid(win32_input(&denied->SidStart), sid)) return true;
    }
    return false;
}

std::vector<std::byte> make_denied_acl(const Backup& backup, PSID sid) {
    const DescriptorParts old = descriptor_parts(backup.descriptor);
    const DWORD sid_size = GetLengthSid(sid);
    if (sid_size == 0) throw_win32(GetLastError());
    const DWORD deny_size =
        static_cast<DWORD>(sizeof(ACCESS_DENIED_ACE) - sizeof(DWORD)) + sid_size;
    DWORD revision = ACL_REVISION;
    DWORD existing_size = sizeof(ACL);
    DWORD existing_count = 0;
    if (old.dacl != nullptr) {
        ACL_SIZE_INFORMATION size{};
        ACL_REVISION_INFORMATION version{};
        if (!GetAclInformation(old.dacl, &size, sizeof(size), AclSizeInformation) ||
            !GetAclInformation(old.dacl, &version, sizeof(version), AclRevisionInformation)) {
            throw_win32(GetLastError());
        }
        existing_size = size.AclBytesInUse;
        existing_count = size.AceCount;
        revision = std::max<DWORD>(version.AclRevision, ACL_REVISION);
    }
    if (existing_size > MAXWORD || deny_size > MAXWORD - existing_size) {
        throw std::runtime_error("resulting access-control list is too large");
    }
    std::vector<std::byte> acl_data(existing_size + deny_size);
    auto* new_acl = reinterpret_cast<PACL>(acl_data.data());
    if (!InitializeAcl(new_acl, static_cast<DWORD>(acl_data.size()), revision) ||
        !AddAccessDeniedAceEx(new_acl, revision, NO_INHERITANCE, FILE_EXECUTE, sid)) {
        throw_win32(GetLastError());
    }
    for (DWORD index = 0; index < existing_count; ++index) {
        void* ace = nullptr;
        if (!GetAce(old.dacl, index, &ace)) throw_win32(GetLastError());
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (!AddAce(new_acl, revision, MAXDWORD, ace, header->AceSize)) {
            throw_win32(GetLastError());
        }
    }
    return acl_data;
}

void verify_protection(const Backup& backup, const HANDLE handle) {
    verify_denied(backup, handle);

    const UniqueHandle readable = probe_access(backup.path, GENERIC_READ);
    if (readable.get() == nullptr) {
        throw std::runtime_error("ordinary read access was unexpectedly blocked: " +
                                 windows_error(GetLastError()));
    }
    SetLastError(ERROR_SUCCESS);
    const UniqueHandle executable = probe_access(backup.path, GENERIC_EXECUTE);
    if (executable.get() != nullptr) {
        throw std::runtime_error("execute access remained available after applying the deny ACE");
    }
    const DWORD error = GetLastError();
    if (error != ERROR_ACCESS_DENIED) {
        throw std::runtime_error("could not verify the execute-only deny ACE: " + windows_error(error));
    }
}

void deny(const Backup& backup, const HANDLE handle) {
    const DescriptorParts old = descriptor_parts(backup.descriptor);
    PACL denied_acl = acl_from_bytes(backup.denied_acl);
    const DWORD status = SetSecurityInfo(handle, SE_FILE_OBJECT, old.information,
                                         nullptr, nullptr, denied_acl, nullptr);
    if (status != ERROR_SUCCESS) throw_win32(status);
    verify_protection(backup, handle);
}

bool acl_equal(PACL left, PACL right) {
    if (left == nullptr || right == nullptr) return left == right;
    ACL_REVISION_INFORMATION left_revision{};
    ACL_REVISION_INFORMATION right_revision{};
    ACL_SIZE_INFORMATION left_size{};
    ACL_SIZE_INFORMATION right_size{};
    if (!GetAclInformation(left, &left_revision, sizeof(left_revision), AclRevisionInformation) ||
        !GetAclInformation(right, &right_revision, sizeof(right_revision), AclRevisionInformation) ||
        !GetAclInformation(left, &left_size, sizeof(left_size), AclSizeInformation) ||
        !GetAclInformation(right, &right_size, sizeof(right_size), AclSizeInformation)) {
        throw_win32(GetLastError());
    }
    if (left_revision.AclRevision != right_revision.AclRevision ||
        left_size.AceCount != right_size.AceCount) {
        return false;
    }
    for (DWORD index = 0; index < left_size.AceCount; ++index) {
        void* left_ace = nullptr;
        void* right_ace = nullptr;
        if (!GetAce(left, index, &left_ace) || !GetAce(right, index, &right_ace)) {
            throw_win32(GetLastError());
        }
        const auto* left_header = static_cast<const ACE_HEADER*>(left_ace);
        const auto* right_header = static_cast<const ACE_HEADER*>(right_ace);
        if (left_header->AceSize != right_header->AceSize ||
            std::memcmp(left_ace, right_ace, left_header->AceSize) != 0) {
            return false;
        }
    }
    return true;
}

bool explicit_acl_equal(PACL left, PACL right) {
    if (left == nullptr || right == nullptr) return left == right;
    ACL_SIZE_INFORMATION left_size{};
    ACL_SIZE_INFORMATION right_size{};
    if (!GetAclInformation(left, &left_size, sizeof(left_size), AclSizeInformation)) {
        throw_win32(GetLastError());
    }
    if (!GetAclInformation(right, &right_size, sizeof(right_size), AclSizeInformation)) {
        throw_win32(GetLastError());
    }

    DWORD left_index = 0;
    DWORD right_index = 0;
    for (;;) {
        void* left_ace = nullptr;
        while (left_index < left_size.AceCount) {
            if (!GetAce(left, left_index++, &left_ace)) throw_win32(GetLastError());
            if ((static_cast<const ACE_HEADER*>(left_ace)->AceFlags & INHERITED_ACE) == 0) break;
            left_ace = nullptr;
        }

        void* right_ace = nullptr;
        while (right_index < right_size.AceCount) {
            if (!GetAce(right, right_index++, &right_ace)) throw_win32(GetLastError());
            if ((static_cast<const ACE_HEADER*>(right_ace)->AceFlags & INHERITED_ACE) == 0) break;
            right_ace = nullptr;
        }

        if (left_ace == nullptr || right_ace == nullptr) return left_ace == right_ace;
        const auto* left_header = static_cast<const ACE_HEADER*>(left_ace);
        const auto* right_header = static_cast<const ACE_HEADER*>(right_ace);
        if (left_header->AceSize != right_header->AceSize ||
            std::memcmp(left_ace, right_ace, left_header->AceSize) != 0) {
            return false;
        }
    }
}

std::vector<std::byte> current_descriptor(const HANDLE handle) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, nullptr, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) throw_win32(status);
    LocalMemory memory(descriptor);
    const DWORD length = GetSecurityDescriptorLength(descriptor);
    if (length == 0 || length > max_state) {
        throw std::runtime_error("invalid current security descriptor length");
    }
    std::vector<std::byte> bytes(length);
    std::memcpy(bytes.data(), descriptor, length);
    static_cast<void>(descriptor_parts(bytes));
    return bytes;
}

bool dacl_state_matches(PACL expected, const SECURITY_DESCRIPTOR_CONTROL expected_control,
                        const DescriptorParts& actual) {
    if (((expected_control ^ actual.control) & SE_DACL_PROTECTED) != 0) return false;
    const bool protected_dacl = (expected_control & SE_DACL_PROTECTED) != 0;
    return protected_dacl
        ? acl_equal(expected, actual.dacl)
        : explicit_acl_equal(expected, actual.dacl);
}

enum class RestorationState {
    denied,
    restored,
};

RestorationState restoration_state(const Backup& backup, const HANDLE handle) {
    const DescriptorParts original = descriptor_parts(backup.descriptor);
    PACL denied = acl_from_bytes(backup.denied_acl);
    const std::vector<std::byte> bytes = current_descriptor(handle);
    const DescriptorParts actual = descriptor_parts(bytes);
    if (dacl_state_matches(original.dacl, original.control, actual)) {
        return RestorationState::restored;
    }
    if (dacl_state_matches(denied, original.control, actual)) {
        return RestorationState::denied;
    }
    throw std::runtime_error(
        "renderer DACL changed outside D2SteamFix; recovery state was preserved");
}

void verify_denied(const Backup& backup, const HANDLE handle) {
    if (restoration_state(backup, handle) != RestorationState::denied) {
        throw std::runtime_error("renderer DACL did not match the applied protection state");
    }
}

void verify_restored(const Backup& backup, const HANDLE handle) {
    if (restoration_state(backup, handle) != RestorationState::restored) {
        throw std::runtime_error("restored DACL did not match its backup");
    }
}

bool restore_one(const Backup& backup, const HANDLE handle) {
    if (restoration_state(backup, handle) == RestorationState::restored) return false;
    const DescriptorParts original = descriptor_parts(backup.descriptor);
    const DWORD status = SetSecurityInfo(handle, SE_FILE_OBJECT, original.information,
                                         nullptr, nullptr, original.dacl, nullptr);
    if (status != ERROR_SUCCESS) throw_win32(status);
    verify_restored(backup, handle);
    return true;
}

void restore_all(const std::span<const Backup> backups,
                 const std::span<const UniqueHandle> handles) {
    if (backups.size() != handles.size()) throw std::logic_error("ACL backup handle mismatch");
    std::exception_ptr first;
    for (size_t index = 0; index < backups.size(); ++index) {
        try {
            restore_one(backups[index], handles[index].get());
        } catch (...) {
            if (!first) first = std::current_exception();
        }
    }
    if (first) std::rethrow_exception(first);
}

void put_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void put_size(std::vector<std::byte>& output, const size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("recovery field too large");
    }
    put_u32(output, static_cast<std::uint32_t>(value));
}

void put_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

std::vector<std::byte> encode_backups(const std::span<const Backup> backups,
                                      const std::span<const FileIdentity> identities) {
    if (backups.size() != identities.size()) throw std::logic_error("ACL backup identity mismatch");
    std::vector<std::byte> output(payload_magic.begin(), payload_magic.end());
    put_size(output, backups.size());
    for (size_t index = 0; index < backups.size(); ++index) {
        const Backup& backup = backups[index];
        const std::wstring path = backup.path.native();
        validate_utf16(path);
        put_size(output, path.size());
        for (const wchar_t unit : path) {
            const auto value = static_cast<std::uint16_t>(unit);
            output.push_back(static_cast<std::byte>(value & 0xffU));
            output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
        }
        put_u32(output, identities[index].volume_serial);
        put_u64(output, identities[index].file_index);
        put_size(output, backup.descriptor.size());
        output.insert(output.end(), backup.descriptor.begin(), backup.descriptor.end());
        static_cast<void>(acl_from_bytes(backup.denied_acl));
        put_size(output, backup.denied_acl.size());
        output.insert(output.end(), backup.denied_acl.begin(), backup.denied_acl.end());
    }
    return output;
}

std::vector<std::byte> protect_state(const std::span<const std::byte> plaintext) {
    if (plaintext.empty() || plaintext.size() > MAXDWORD) {
        throw std::runtime_error("invalid recovery plaintext size");
    }
    DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                    static_cast<BYTE*>(win32_input(plaintext.data()))};
    DATA_BLOB protected_data{};
    if (!CryptProtectData(&input, L"D2SteamFix ACL recovery", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &protected_data)) {
        throw_win32(GetLastError());
    }
    LocalMemory memory(protected_data.pbData);
    if (protected_data.cbData == 0 || protected_data.cbData > max_state_file - state_magic.size()) {
        throw std::runtime_error("invalid protected recovery size");
    }
    std::vector<std::byte> output(state_magic.begin(), state_magic.end());
    const auto begin = reinterpret_cast<const std::byte*>(protected_data.pbData);
    const auto protected_bytes = std::span(begin, protected_data.cbData);
    output.insert(output.end(), protected_bytes.begin(), protected_bytes.end());
    return output;
}

std::vector<std::byte> unprotect_state(const std::span<const std::byte> protected_data) {
    if (protected_data.empty() || protected_data.size() > MAXDWORD) {
        throw std::runtime_error("invalid protected recovery size");
    }
    DATA_BLOB input{static_cast<DWORD>(protected_data.size()),
                    static_cast<BYTE*>(win32_input(protected_data.data()))};
    DATA_BLOB plaintext{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &plaintext)) {
        throw_win32(GetLastError());
    }
    LocalMemory memory(plaintext.pbData);
    if (plaintext.cbData == 0 || plaintext.cbData > max_state) {
        throw std::runtime_error("invalid recovery plaintext size");
    }
    const auto begin = reinterpret_cast<const std::byte*>(plaintext.pbData);
    const auto plaintext_bytes = std::span(begin, plaintext.cbData);
    return std::vector<std::byte>(plaintext_bytes.begin(), plaintext_bytes.end());
}

std::uint32_t take_u32(const std::span<const std::byte> data, size_t& offset) {
    require_range(data, offset, sizeof(std::uint32_t), "truncated recovery file");
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        result |= std::to_integer<std::uint32_t>(data[offset++]) << shift;
    }
    return result;
}

std::uint64_t take_u64(const std::span<const std::byte> data, size_t& offset) {
    require_range(data, offset, sizeof(std::uint64_t), "truncated recovery file");
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        result |= std::to_integer<std::uint64_t>(data[offset++]) << shift;
    }
    return result;
}

struct RecoveryEntry {
    Backup backup;
    FileIdentity identity;
};

std::vector<RecoveryEntry> decode_payload(const std::span<const std::byte> data) {
    if (data.size() < payload_magic.size() + sizeof(std::uint32_t) ||
        !std::equal(payload_magic.begin(), payload_magic.end(), data.begin())) {
        throw std::runtime_error("invalid recovery file header");
    }
    size_t offset = payload_magic.size();
    const std::uint32_t count = take_u32(data, offset);
    if (count == 0 || count > max_entries) {
        throw std::runtime_error("invalid recovery entry count");
    }
    std::vector<RecoveryEntry> result;
    result.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t units = take_u32(data, offset);
        if (units == 0 || units > 32767U) {
            throw std::runtime_error("invalid recovery path length");
        }
        const size_t path_bytes = static_cast<size_t>(units) * sizeof(std::uint16_t);
        require_range(data, offset, path_bytes, "truncated recovery path");
        std::wstring path;
        path.reserve(units);
        for (std::uint32_t unit = 0; unit < units; ++unit) {
            const std::uint16_t low = std::to_integer<std::uint8_t>(data[offset++]);
            const std::uint16_t high = std::to_integer<std::uint8_t>(data[offset++]);
            path.push_back(static_cast<wchar_t>(low | (high << 8U)));
        }
        validate_utf16(path);
        const fs::path target(path);
        validate_target_name(target);

        const FileIdentity identity{take_u32(data, offset), take_u64(data, offset)};

        const std::uint32_t descriptor_length = take_u32(data, offset);
        if (descriptor_length == 0 || descriptor_length > max_state) {
            throw std::runtime_error("invalid descriptor length");
        }
        const size_t descriptor_end = checked_add(offset, descriptor_length);
        if (descriptor_end > data.size()) throw std::runtime_error("truncated descriptor");
        const auto descriptor_bytes = data.subspan(offset, descriptor_length);
        std::vector<std::byte> descriptor(descriptor_bytes.begin(), descriptor_bytes.end());
        offset = descriptor_end;
        static_cast<void>(descriptor_parts(descriptor));

        const std::uint32_t denied_length = take_u32(data, offset);
        if (denied_length < sizeof(ACL) || denied_length > MAXWORD) {
            throw std::runtime_error("invalid protected ACL length");
        }
        const size_t denied_end = checked_add(offset, denied_length);
        if (denied_end > data.size()) throw std::runtime_error("truncated protected ACL");
        const auto denied_bytes = data.subspan(offset, denied_length);
        std::vector<std::byte> denied_acl(denied_bytes.begin(), denied_bytes.end());
        offset = denied_end;
        static_cast<void>(acl_from_bytes(denied_acl));
        result.push_back(
            {Backup{target, std::move(descriptor), std::move(denied_acl)}, identity});
    }
    if (offset != data.size()) throw std::runtime_error("trailing recovery data");
    return result;
}

std::vector<RecoveryEntry> decode_backups(const std::span<const std::byte> data) {
    if (data.size() <= state_magic.size() ||
        !std::equal(state_magic.begin(), state_magic.end(), data.begin())) {
        throw std::runtime_error("invalid recovery file header");
    }
    const std::vector<std::byte> plaintext = unprotect_state(data.subspan(state_magic.size()));
    return decode_payload(plaintext);
}

void write_all(const HANDLE file, const std::span<const std::byte> data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const size_t remaining = data.size() - offset;
        const DWORD requested = static_cast<DWORD>(std::min<size_t>(remaining, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(file, data.subspan(offset, requested).data(), requested, &written, nullptr)) {
            throw_win32(GetLastError());
        }
        if (written == 0) throw std::runtime_error("recovery file write made no progress");
        offset += written;
    }
}

void write_state(const fs::path& path, const std::span<const Backup> backups,
                 const std::span<const FileIdentity> identities, PSID user_sid) {
    const std::vector<std::byte> plaintext = encode_backups(backups, identities);
    if (plaintext.size() > max_state) throw std::runtime_error("recovery state is too large");
    const std::vector<std::byte> data = protect_state(plaintext);
    if (data.size() > max_state_file) throw std::runtime_error("recovery state is too large");
    fs::path temporary = path;
    temporary += L".tmp";
    RestrictedSecurity security(user_sid, 0);
    UniqueHandle file = elevation::open_file(temporary, GENERIC_WRITE, 0, CREATE_NEW,
                                               FILE_ATTRIBUTE_NORMAL, security.attributes());
    try {
        write_all(file.get(), data);
        if (!FlushFileBuffers(file.get())) throw_win32(GetLastError());
        file.reset();
        elevation::move_recovery_file(temporary, path);
    } catch (...) {
        file.reset();
        try { elevation::remove_recovery_file(temporary); } catch (...) {}
        throw;
    }
}

std::vector<RecoveryEntry> read_state(const fs::path& path) {
    UniqueHandle file = elevation::open_file(path, GENERIC_READ | READ_CONTROL | WRITE_DAC,
                                               0, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.get(), &information)) throw_win32(GetLastError());
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        throw std::runtime_error("recovery state is not a regular, non-reparse file");
    }
    UserIdentity identity = current_user();
    require_current_user_owner(file.get(), identity.sid());
    RestrictedSecurity security(identity.sid(), 0);
    const DWORD acl_status = SetSecurityInfo(
        file.get(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, security.acl(), nullptr);
    if (acl_status != ERROR_SUCCESS) throw_win32(acl_status);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) throw_win32(GetLastError());
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_state_file) {
        throw std::runtime_error("recovery file is too large");
    }
    std::vector<std::byte> data(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < data.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<size_t>(data.size() - offset, MAXDWORD));
        DWORD read = 0;
        if (!ReadFile(file.get(), std::span(data).subspan(offset, requested).data(),
                       requested, &read, nullptr)) {
            throw_win32(GetLastError());
        }
        if (read == 0) throw std::runtime_error("could not read complete recovery file");
        offset += read;
    }
    return decode_backups(data);
}

AppError protection_error(const char* stage, const std::exception& error) {
    if (dynamic_cast<const elevation::Cancelled*>(&error) != nullptr) throw elevation::Cancelled();
    return AppError(ExitCode::overlay_protection, stage, error.what());
}

void remove_incomplete_state(const fs::path& path, const Logger& log) {
    fs::path temporary = path;
    temporary += L".tmp";
    std::error_code error;
    const bool exists = fs::exists(temporary, error);
    if (error) {
        throw protection_error("ACL recovery",
                               fs::filesystem_error("inspect temporary recovery file", temporary, error));
    }
    if (exists) {
        try {
            if (!elevation::remove_recovery_file(temporary)) {
                throw std::runtime_error("could not remove temporary recovery file");
            }
        } catch (const std::exception& exception) {
            throw protection_error("ACL recovery cleanup", exception);
        }
        log.warn("removed an incomplete ACL recovery temporary file");
    }
}

struct PreparedRecovery {
    const RecoveryEntry* entry;
    OpenedTarget observed;
    OpenedTarget writable;
};

std::optional<PreparedRecovery> prepare_recovery_entry(const RecoveryEntry& entry,
                                                       const Logger& log) {
    const std::string shown = display_path(entry.backup.path);
    std::optional<OpenedTarget> observed;
    try {
        observed = open_target(entry.backup.path, FILE_READ_ATTRIBUTES);
    } catch (const UnsafeTarget& error) {
        log.warn("skipped ACL recovery for unsafe renderer target: " + shown + ": " + error.what());
        return std::nullopt;
    } catch (const std::system_error& error) {
        if (missing_error(static_cast<DWORD>(error.code().value()))) {
            log.warn("skipped ACL recovery for missing renderer: " + shown);
            return std::nullopt;
        }
        throw;
    }
    if (observed->identity != entry.identity) {
        log.warn("skipped ACL recovery because renderer identity changed: " + shown);
        return std::nullopt;
    }
    if (!has_regular_steam_neighbor(entry.backup.path)) {
        log.warn("skipped ACL recovery because steam.exe is missing or unsafe: " + shown);
        return std::nullopt;
    }
    OpenedTarget writable = open_target(
        entry.backup.path, READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES);
    if (writable.identity != entry.identity || writable.identity != observed->identity) {
        throw std::runtime_error("renderer identity changed while ACL recovery was opening it: " + shown);
    }
    static_cast<void>(restoration_state(entry.backup, writable.handle.get()));
    return PreparedRecovery{&entry, std::move(*observed), std::move(writable)};
}

} // namespace

fs::path recovery_path(const fs::path& game_directory) {
    return game_directory / L"steamfix.dat";
}

bool recover_pending(const fs::path& path, const Logger& log) {
    remove_incomplete_state(path, log);
    std::error_code error;
    const bool state_exists = fs::exists(path, error);
    if (error) {
        throw protection_error("ACL recovery",
                               fs::filesystem_error("inspect recovery file", path, error));
    }
    if (!state_exists) return false;
    try {
        const std::vector<RecoveryEntry> entries = read_state(path);
        log.warn("found pending ACL recovery state with " +
                  std::to_string(entries.size()) + " entries");
        std::vector<PreparedRecovery> prepared;
        prepared.reserve(entries.size());
        size_t skipped = 0;
        for (const RecoveryEntry& entry : entries) {
            std::optional<PreparedRecovery> target = prepare_recovery_entry(entry, log);
            if (target) {
                prepared.push_back(std::move(*target));
            } else {
                ++skipped;
            }
        }
        if (skipped != 0) {
            log.warn("preflighted renderer permission recovery state: prepared=" +
                     std::to_string(prepared.size()) + ", skipped=" + std::to_string(skipped) +
                     "; recovery state preserved");
            throw std::runtime_error(
                "one or more renderer files could not be safely matched; recovery state was preserved");
        }
        size_t restored = 0;
        for (const PreparedRecovery& target : prepared) {
            if (restore_one(target.entry->backup, target.writable.handle.get())) ++restored;
        }
        const bool removed = elevation::remove_recovery_file(path);
        if (!removed) throw std::runtime_error("recovery file disappeared before cleanup");
        log.info("processed renderer permission recovery state: restored=" +
                 std::to_string(restored) + ", skipped=" + std::to_string(skipped));
        return true;
    } catch (const AppError&) {
        throw;
    } catch (const std::exception& exception) {
        throw protection_error("ACL recovery", exception);
    }
}

Protection Protection::apply(const std::span<const fs::path> targets, fs::path state_path,
                             const Logger& log) {
    if (targets.empty() || targets.size() > max_entries) {
        throw AppError(ExitCode::overlay_protection, "ACL backup",
                       "expected one or two renderer targets");
    }
    if (targets.size() == 2 && paths::equal_windows(targets.front(), targets.back())) {
        throw AppError(ExitCode::overlay_protection, "ACL backup",
                       "renderer targets must refer to distinct files");
    }
    std::vector<Backup> backups;
    std::vector<UniqueHandle> handles;
    std::vector<FileIdentity> identities;
    bool resuming = false;
    try {
        backups.reserve(targets.size());
        handles.reserve(targets.size());
        identities.reserve(targets.size());
        resuming = fs::exists(state_path);
        if (resuming) {
            std::vector<RecoveryEntry> entries = read_state(state_path);
            if (entries.size() != targets.size()) {
                throw std::runtime_error(
                    "saved renderer set does not match the selected Steam renderer files; "
                    "original recovery backup was preserved");
            }
            std::vector<bool> matched(entries.size(), false);
            for (const fs::path& target : targets) {
                size_t match = entries.size();
                for (size_t index = 0; index < entries.size(); ++index) {
                    if (!paths::equal_windows(target, entries[index].backup.path)) continue;
                    if (matched[index] || match != entries.size()) {
                        throw std::runtime_error("saved renderer set contains duplicate targets");
                    }
                    match = index;
                }
                if (match == entries.size()) {
                    throw std::runtime_error("selected renderer is missing from the original "
                                             "recovery backup: " + display_path(target));
                }
                try {
                    std::optional<PreparedRecovery> prepared =
                        prepare_recovery_entry(entries[match], log);
                    if (!prepared) {
                        throw std::runtime_error("renderer could not be safely matched to its "
                                                 "original recovery backup");
                    }
                    matched[match] = true;
                    backups.push_back(entries[match].backup);
                    handles.push_back(std::move(prepared->writable.handle));
                } catch (const elevation::Cancelled&) {
                    throw;
                } catch (const std::exception& error) {
                    throw std::runtime_error(display_path(target) + ": " + error.what());
                }
            }
        } else {
            for (const fs::path& target : targets) {
                OpenBackup opened = make_backup(target);
                backups.push_back(std::move(opened.backup));
                handles.push_back(std::move(opened.handle));
                identities.push_back(opened.identity);
            }
        }
    } catch (const std::exception& error) {
        throw protection_error(resuming ? "ACL recovery validation" : "ACL backup", error);
    }
    UserIdentity identity = [] {
        try {
            return current_user();
        } catch (const std::exception& error) {
            throw protection_error("current-user identity", error);
        }
    }();
    try {
        if (!resuming) {
            for (Backup& backup : backups) {
                if (has_current_user_execute_deny(backup, identity.sid())) {
                    throw std::runtime_error(
                        "current-user execute access is already denied but no matching recovery "
                        "backup was found; preserve any existing steamfix.dat files: " +
                        display_path(backup.path));
                }
                backup.denied_acl = make_denied_acl(backup, identity.sid());
            }
        }
    } catch (const std::exception& error) {
        throw protection_error("ACL protection preparation", error);
    }
    try {
        if (!resuming) {
            remove_incomplete_state(state_path, log);
            write_state(state_path, backups, identities, identity.sid());
        }
    } catch (const AppError&) {
        throw;
    } catch (const std::exception& error) {
        throw protection_error("ACL recovery persistence", error);
    }
    if (resuming) log.info("reusing original renderer permissions from existing recovery backup");
    for (size_t index = 0; index < backups.size(); ++index) {
        try {
            if (restoration_state(backups[index], handles[index].get()) == RestorationState::denied) {
                verify_protection(backups[index], handles[index].get());
                log.info("retained current-user execute denial from interrupted session: " +
                         display_path(backups[index].path));
            } else {
                deny(backups[index], handles[index].get());
                log.info("temporarily denied current-user execute access: " +
                         display_path(backups[index].path));
            }
        } catch (const std::exception& error) {
            bool restored = false;
            try {
                restore_all(backups, handles);
                restored = true;
            } catch (const std::exception&) {
            }
            if (restored) {
                try { elevation::remove_recovery_file(state_path); } catch (...) {}
            }
            throw AppError(ExitCode::overlay_protection, "renderer permission denial",
                           display_path(backups[index].path) + ": " + error.what());
        }
    }
    return Protection(std::move(backups), std::move(handles), std::move(state_path));
}

void Protection::restore(const Logger& log) {
    if (state_ != State::active) return;
    try {
        restore_all(backups_, handles_);
    } catch (const std::exception& error) {
        throw protection_error("ACL restoration", error);
    }
    // Permission restoration is complete even if journal cleanup is declined.
    // Do not repeat a cancelled elevation prompt from the destructor.
    state_ = State::restored;
    try {
        elevation::remove_recovery_file(state_path_);
    } catch (const std::exception& error) {
        throw protection_error("ACL recovery cleanup", error);
    }
    log.info("restored original renderer permissions and removed recovery state");
}

Protection::~Protection() noexcept {
    if (state_ != State::active) return;
    try {
        restore_all(backups_, handles_);
        elevation::remove_recovery_file(state_path_);
        state_ = State::restored;
    } catch (...) {
    }
}

} // namespace steamfix::acl
