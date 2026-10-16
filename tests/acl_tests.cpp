// Load production dependencies before redirecting the three selected Win32
// call sites in acl.cpp. Declarations and separately compiled modules stay real.
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
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace acl_fault {

struct Plan {
    int write_failures_remaining = 0;
    int flush_failures_remaining = 0;
    std::array<int, 2> security_failure_calls{};
    int write_failures = 0;
    int flush_failures = 0;
    int security_calls = 0;
    int security_failures = 0;
};

// Each plan is borrowed only by the current test thread's bounded Scope.
thread_local Plan* active_plan = nullptr;

BOOL WINAPI write_file(const HANDLE file, const LPCVOID bytes, const DWORD length,
                       const LPDWORD written, const LPOVERLAPPED overlap) {
    if (active_plan != nullptr && active_plan->write_failures_remaining != 0) {
        --active_plan->write_failures_remaining;
        ++active_plan->write_failures;
        if (written != nullptr) *written = 0;
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return ::WriteFile(file, bytes, length, written, overlap);
}

BOOL WINAPI flush_file(const HANDLE file) {
    if (active_plan != nullptr && active_plan->flush_failures_remaining != 0) {
        --active_plan->flush_failures_remaining;
        ++active_plan->flush_failures;
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return ::FlushFileBuffers(file);
}

DWORD WINAPI set_security(const HANDLE handle, const SE_OBJECT_TYPE type,
                          const SECURITY_INFORMATION information, const PSID owner,
                          const PSID group, const PACL dacl, const PACL sacl) {
    if (active_plan != nullptr) {
        ++active_plan->security_calls;
        for (const int failure_call : active_plan->security_failure_calls) {
            if (active_plan->security_calls == failure_call) {
                ++active_plan->security_failures;
                return ERROR_WRITE_FAULT;
            }
        }
    }
    return ::SetSecurityInfo(handle, type, information, owner, group, dacl, sacl);
}

class Scope final {
public:
    explicit Scope(Plan& plan) noexcept : previous_(active_plan) { active_plan = &plan; }
    ~Scope() noexcept { active_plan = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
private:
    Plan* previous_;
};

} // namespace acl_fault

// Keep actual binary readers and ACL/recovery state transitions in the test TU.
// With no active plan, every call below invokes the corresponding real API.
#define WriteFile acl_fault::write_file
#define FlushFileBuffers acl_fault::flush_file
#define SetSecurityInfo acl_fault::set_security
#include "../src/acl.cpp"
#undef WriteFile
#undef FlushFileBuffers
#undef SetSecurityInfo

namespace {
namespace acl = steamfix::acl;
using steamfix::UniqueHandle;

static_assert(!std::is_copy_constructible_v<acl::Protection> &&
              !std::is_copy_assignable_v<acl::Protection> &&
              !std::is_move_constructible_v<acl::Protection> &&
              !std::is_move_assignable_v<acl::Protection> &&
              std::is_nothrow_destructible_v<acl::Protection>);
static_assert(!std::is_copy_constructible_v<acl::RestrictedSecurity> &&
              !std::is_copy_assignable_v<acl::RestrictedSecurity> &&
              !std::is_move_constructible_v<acl::RestrictedSecurity> &&
              !std::is_move_assignable_v<acl::RestrictedSecurity>);

std::size_t assertions = 0;

void require(const bool condition, const std::string_view message) {
    ++assertions;
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Exception = std::exception, typename Action>
    requires std::derived_from<Exception, std::exception> && std::invocable<Action>
void rejects(Action action, const std::string_view message) {
    ++assertions;
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void native_require(const BOOL result, const char* message) {
    if (!result) throw std::system_error(steamfix::windows_exit_status(GetLastError()),
                                         std::system_category(), message);
}

class TempDirectory final {
public:
    TempDirectory() {
        const fs::path parent = fs::temp_directory_path();
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            path_ = parent / (L"D2SteamFix-acl-tests-" + std::to_wstring(GetCurrentProcessId()) +
                              L"-" + std::to_wstring(GetTickCount64()) + L"-" +
                              std::to_wstring(attempt));
            if (CreateDirectoryW(path_.c_str(), nullptr)) return;
            if (GetLastError() != ERROR_ALREADY_EXISTS) native_require(FALSE, "create test directory");
        }
        throw std::runtime_error("could not allocate a unique test directory");
    }
    ~TempDirectory() noexcept {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void create_file(const fs::path& path, const std::span<const std::byte> contents = {}) {
    acl::UserIdentity user = acl::current_user();
    acl::RestrictedSecurity security(user.sid(), 0);
    const UniqueHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ |
                                        FILE_SHARE_WRITE | FILE_SHARE_DELETE, security.attributes(),
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    acl::write_all(file.get(), contents);
}

std::vector<std::byte> file_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "could not open test fixture for comparison");
    const std::vector<char> contents{std::istreambuf_iterator<char>{file},
                                     std::istreambuf_iterator<char>{}};
    require(!file.bad(), "could not read test fixture for comparison");
    const auto bytes = std::as_bytes(std::span(contents));
    return {bytes.begin(), bytes.end()};
}

struct Fixture {
    TempDirectory directory;
    fs::path renderer = directory.path() / L"GameOverlayRenderer64.dll";
    fs::path renderer32 = directory.path() / L"GameOverlayRenderer.dll";
    fs::path state = acl::recovery_path(directory.path());
    steamfix::Logger log{directory.path() / L"steamfix.log"};

    Fixture() {
        create_file(directory.path() / L"steam.exe");
        create_file(directory.path() / L"destiny2.exe");
        create_file(directory.path() / L"destiny2launcher.exe");
        constexpr std::array contents{std::byte{0x4d}, std::byte{0x5a}, std::byte{0x00}};
        create_file(renderer, contents);
        create_file(renderer32, contents);
    }
};

std::vector<std::byte> binary(const std::initializer_list<unsigned char> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) result.push_back(std::byte{value});
    return result;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
void replace_object(std::vector<std::byte>& bytes, const std::size_t offset, const T& value) {
    require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset,
            "test mutation exceeded its fixture");
    std::memcpy(std::span(bytes).subspan(offset, sizeof(T)).data(), &value, sizeof(T));
}

template <typename Action>
    requires std::invocable<Action, SECURITY_DESCRIPTOR_RELATIVE&>
std::vector<std::byte> changed_descriptor(const std::vector<std::byte>& original, Action action) {
    auto changed = original;
    auto header = acl::object_at<SECURITY_DESCRIPTOR_RELATIVE>(changed, 0, "test descriptor");
    action(header);
    replace_object(changed, 0, header);
    return changed;
}

std::vector<std::byte> relative_descriptor(PACL dacl, const bool present = true,
                                          const bool protected_dacl = true,
                                          PSID owner = nullptr, PACL sacl = nullptr) {
    SECURITY_DESCRIPTOR descriptor{};
    native_require(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION),
                   "initialize descriptor fixture");
    native_require(SetSecurityDescriptorDacl(&descriptor, present ? TRUE : FALSE, dacl, FALSE),
                   "set descriptor DACL fixture");
    if (protected_dacl) {
        native_require(SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED,
                                                    SE_DACL_PROTECTED), "protect descriptor fixture");
    }
    if (owner != nullptr) {
        native_require(SetSecurityDescriptorOwner(&descriptor, owner, FALSE), "set fixture owner");
        native_require(SetSecurityDescriptorGroup(&descriptor, owner, FALSE), "set fixture group");
    }
    if (sacl != nullptr) {
        native_require(SetSecurityDescriptorSacl(&descriptor, TRUE, sacl, FALSE), "set fixture SACL");
    }
    DWORD size = 0;
    static_cast<void>(MakeSelfRelativeSD(&descriptor, nullptr, &size));
    require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && size >= sizeof(SECURITY_DESCRIPTOR_RELATIVE),
            "native descriptor sizing failed");
    std::vector<std::byte> result(size);
    native_require(MakeSelfRelativeSD(&descriptor, result.data(), &size), "serialize descriptor fixture");
    return result;
}

acl::OpenBackup prepared_backup(const fs::path& path) {
    auto result = acl::make_backup(path);
    auto user = acl::current_user();
    result.backup.denied_acl = acl::make_denied_acl(result.backup, user.sid());
    return result;
}

void force_original(const acl::OpenBackup& opened) {
    const auto original = acl::descriptor_parts(opened.backup.descriptor);
    const DWORD error = SetSecurityInfo(opened.handle.get(), SE_FILE_OBJECT, original.information,
                                        nullptr, nullptr, original.dacl, nullptr);
    if (error != ERROR_SUCCESS) acl::throw_win32(error);
}

void binary_and_utf16_tests() {
    require(acl::checked_add(3, 4) == 7, "checked size addition changed a valid value");
    rejects([&] { static_cast<void>(acl::checked_add(std::numeric_limits<size_t>::max(), 1)); },
            "overflowed recovery offsets were accepted");
    const auto integers = binary({0x78, 0x56, 0x34, 0x12, 0x88, 0x77, 0x66, 0x55,
                                  0x44, 0x33, 0x22, 0x11});
    std::size_t offset = 0;
    require(acl::take_u32(integers, offset) == 0x12345678U && offset == 4,
            "u32 reader lost little-endian ordering or offset");
    require(acl::take_u64(integers, offset) == 0x1122334455667788ULL && offset == integers.size(),
            "u64 reader lost little-endian ordering or offset");
    for (std::size_t length = 0; length < 4; ++length) {
        offset = 0;
        rejects([&] { static_cast<void>(acl::take_u32(std::span(integers).first(length), offset)); },
                "truncated integer was accepted");
    }
    offset = integers.size() - 7;
    rejects([&] { static_cast<void>(acl::take_u64(integers, offset)); }, "truncated u64 was accepted");
    std::vector<std::byte> encoded;
    acl::put_u32(encoded, 0x12345678U);
    acl::put_u64(encoded, 0x1122334455667788ULL);
    require(encoded == integers, "integer writer changed the recovery format");
    rejects([&] { acl::put_size(encoded, std::numeric_limits<std::uint32_t>::max() + size_t{1}); },
            "oversized recovery length was narrowed");
    rejects([&] { static_cast<void>(acl::object_at<DWORD>(integers, integers.size(), "truncated")); },
            "object reader accepted an empty tail");

    acl::validate_utf16(L"renderer \U0001f3ae");
    acl::validate_utf16(L"");
    const std::array invalid_text{
        std::wstring(1, wchar_t{0}), std::wstring(1, wchar_t{0xd800}),
        std::wstring(1, wchar_t{0xdc00}), std::wstring{wchar_t{0xd800}, L'x'}};
    for (const auto& text : invalid_text) {
        rejects([&] { acl::validate_utf16(text); }, "invalid UTF-16 recovery path was accepted");
    }
    rejects([&] { acl::validate_target_name(L"GameOverlayRenderer64.dll"); },
            "relative renderer path was accepted");
    rejects([&] { acl::validate_target_name(fs::temp_directory_path() / L"unrelated.dll"); },
            "unrelated recovery target was accepted");
    require(acl::missing_error(ERROR_FILE_NOT_FOUND) && acl::missing_error(ERROR_PATH_NOT_FOUND) &&
            !acl::missing_error(ERROR_ACCESS_DENIED), "missing-file errors were misclassified");
    rejects<steamfix::elevation::Cancelled>([&] {
        static_cast<void>(acl::protection_error("cancelled test", steamfix::elevation::Cancelled{}));
    }, "cancellation was hidden by protection error wrapping");
    const auto wrapped = acl::protection_error("test stage", std::runtime_error("test detail"));
    require(wrapped.code() == steamfix::ExitCode::overlay_protection && wrapped.stage() == "test stage" &&
            wrapped.detail() == "test detail", "protection errors lost their stage or detail");
}

void acl_and_descriptor_tests() {
    // A complete deny ACE with one SID subauthority, independent of the native writer.
    const auto valid = binary({2,0,28,0,1,0,0,0, 1,0,20,0,32,0,0,0,
                               1,1,0,0,0,0,0,5,21,0,0,0});
    acl::validate_acl(valid);
    require(IsValidAcl(acl::acl_from_bytes(valid)) != FALSE, "valid ACL fixture was rejected");
    for (std::size_t length = 0; length < valid.size(); ++length) {
        rejects([&] { acl::validate_acl(std::span(valid).first(length)); },
                "truncated ACL fixture was accepted");
    }
    const std::array corruptions{
        std::pair{std::size_t{0}, std::byte{0}}, // unsupported revision
        std::pair{std::size_t{2}, std::byte{4}}, // ACL smaller than its header
        std::pair{std::size_t{4}, std::byte{2}}, // missing second ACE
        std::pair{std::size_t{10}, std::byte{0}}, // empty ACE
        std::pair{std::size_t{10}, std::byte{19}}, // unaligned ACE
        std::pair{std::size_t{10}, std::byte{24}}, // ACE beyond its ACL
        std::pair{std::size_t{16}, std::byte{2}}, // invalid SID revision
        std::pair{std::size_t{17}, std::byte{2}}, // truncated SID tail
        std::pair{std::size_t{17}, std::byte{255}}};
    for (const auto& [position, value] : corruptions) {
        auto changed = valid;
        changed[position] = value;
        rejects([&] { acl::validate_acl(changed); }, "malformed ACL fixture was accepted");
    }
    const auto header_only = binary({2,0,12,0,1,0,0,0, 1,0,4,0});
    const auto mask_only = binary({2,0,16,0,1,0,0,0, 1,0,8,0,32,0,0,0});
    rejects([&] { acl::validate_acl(header_only); }, "deny ACE without its fields was accepted");
    rejects([&] { acl::validate_acl(mask_only); }, "deny ACE without a SID was accepted");
    std::vector<std::byte> short_acl_in_larger_descriptor(sizeof(SECURITY_DESCRIPTOR_RELATIVE));
    short_acl_in_larger_descriptor.insert(short_acl_in_larger_descriptor.end(), valid.begin(), valid.end());
    replace_object(short_acl_in_larger_descriptor, sizeof(SECURITY_DESCRIPTOR_RELATIVE) + 2U, WORD{12});
    rejects([&] {
        acl::validate_relative_acl(short_acl_in_larger_descriptor, sizeof(SECURITY_DESCRIPTOR_RELATIVE));
    }, "ACE escaped its ACL into the surrounding descriptor");
    auto extra = valid;
    extra.push_back(std::byte{0});
    rejects([&] { static_cast<void>(acl::acl_from_bytes(extra)); }, "ACL trailing bytes were accepted");
    const std::vector<std::byte> oversized_acl(size_t{MAXWORD} + 1U);
    rejects([&] { static_cast<void>(acl::acl_from_bytes(oversized_acl)); }, "oversized ACL was accepted");

    auto user = acl::current_user();
    auto copied_user = user;
    require(EqualSid(user.sid(), copied_user.sid()) != FALSE && user.sid() != copied_user.sid(),
            "copied user identity retained a pointer into its source");
    acl::RestrictedSecurity security(user.sid(), 0);
    const auto descriptor = relative_descriptor(security.acl(), true, true, user.sid(), security.acl());
    acl::validate_relative_descriptor(descriptor);
    const auto parts = acl::descriptor_parts(descriptor);
    require(parts.dacl != nullptr && (parts.information & PROTECTED_DACL_SECURITY_INFORMATION) != 0,
            "protected descriptor inheritance was lost");
    const auto unprotected = relative_descriptor(security.acl(), true, false);
    require((acl::descriptor_parts(unprotected).information & UNPROTECTED_DACL_SECURITY_INFORMATION) != 0,
            "unprotected descriptor inheritance was lost");
    const auto null_dacl = relative_descriptor(nullptr);
    require(acl::descriptor_parts(null_dacl).dacl == nullptr, "null DACL was changed into an empty DACL");
    const auto absent_dacl = relative_descriptor(nullptr, false);
    rejects([&] { static_cast<void>(acl::descriptor_parts(absent_dacl)); }, "absent DACL was accepted");
    const std::array descriptor_changes{
        changed_descriptor(descriptor, [](auto& h) { h.Revision = 0; }),
        changed_descriptor(descriptor, [](auto& h) {
            h.Control = static_cast<SECURITY_DESCRIPTOR_CONTROL>(h.Control & ~SE_SELF_RELATIVE);
        }),
        changed_descriptor(descriptor, [](auto& h) { h.Owner = 4; }),
        changed_descriptor(descriptor, [](auto& h) { h.Group = 21; }),
        changed_descriptor(descriptor, [](auto& h) { h.Dacl = MAXDWORD - 3U; }),
        changed_descriptor(descriptor, [](auto& h) {
            h.Control = static_cast<SECURITY_DESCRIPTOR_CONTROL>(h.Control & ~SE_DACL_PRESENT);
        }),
        changed_descriptor(descriptor, [](auto& h) {
            h.Control = static_cast<SECURITY_DESCRIPTOR_CONTROL>(h.Control & ~SE_SACL_PRESENT);
        })};
    for (const auto& changed : descriptor_changes) {
        rejects([&] { acl::validate_relative_descriptor(changed); }, "malformed descriptor was accepted");
    }
    auto trailing = descriptor;
    trailing.push_back(std::byte{0});
    rejects([&] { acl::validate_relative_descriptor(trailing); }, "descriptor trailing bytes were accepted");
    rejects([&] { acl::validate_relative_descriptor(std::span(descriptor).first(8)); },
            "truncated descriptor header was accepted");
    rejects([&] { acl::validate_relative_acl(descriptor, 21); }, "unaligned ACL offset was accepted");
    rejects([&] { acl::validate_relative_sid(descriptor, MAXDWORD - 3U); },
            "SID offset outside descriptor was accepted");
    require(acl::acl_equal(nullptr, nullptr) && !acl::acl_equal(nullptr, security.acl()) &&
            acl::explicit_acl_equal(nullptr, nullptr) && !acl::explicit_acl_equal(nullptr, security.acl()),
            "null ACL equality is inconsistent");
    require(acl::acl_equal(security.acl(), security.acl()) &&
            acl::explicit_acl_equal(security.acl(), security.acl()), "identical ACLs compared unequal");
    require(!acl::dacl_state_matches(security.acl(), 0, parts), "DACL protection flag was ignored");

    auto changed_mask = valid;
    changed_mask[12] ^= std::byte{1};
    require(!acl::acl_equal(acl::acl_from_bytes(valid), acl::acl_from_bytes(changed_mask)) &&
            !acl::explicit_acl_equal(acl::acl_from_bytes(valid), acl::acl_from_bytes(changed_mask)),
            "different ACE access masks compared equal");
    require(!acl::acl_equal(security.acl(), acl::acl_from_bytes(valid)), "different ACE counts compared equal");
    auto revision4 = valid;
    revision4[0] = std::byte{ACL_REVISION_DS};
    require(!acl::acl_equal(acl::acl_from_bytes(valid), acl::acl_from_bytes(revision4)),
            "different ACL revisions compared equal");
    auto inherited = valid;
    inherited[9] = std::byte{INHERITED_ACE};
    const auto empty = binary({2,0,8,0,0,0,0,0});
    require(acl::explicit_acl_equal(acl::acl_from_bytes(inherited), acl::acl_from_bytes(empty)) &&
            acl::explicit_acl_equal(acl::acl_from_bytes(empty), acl::acl_from_bytes(inherited)),
            "inherited ACEs were compared as explicit permissions");
    require(!acl::explicit_acl_equal(acl::acl_from_bytes(valid), acl::acl_from_bytes(empty)),
            "an explicit ACE compared equal to an empty ACL");
    auto allowed = valid;
    allowed[8] = std::byte{ACCESS_ALLOWED_ACE_TYPE};
    acl::validate_acl(allowed);
    acl::Backup originally_unrestricted{fs::temp_directory_path() / L"GameOverlayRenderer64.dll", null_dacl, {}};
    originally_unrestricted.denied_acl = acl::make_denied_acl(originally_unrestricted, user.sid());
    require(!acl::has_current_user_execute_deny(originally_unrestricted, user.sid()),
            "null DACL was reported as an existing execute denial");
    require(acl::acl_from_bytes(originally_unrestricted.denied_acl)->AceCount == 1,
            "null DACL did not produce exactly one execute-only denial");
}

void serialization_tests() {
    Fixture fixture;
    auto opened = prepared_backup(fixture.renderer);
    const std::array backups{opened.backup};
    const std::array identities{opened.identity};
    const auto plaintext = acl::encode_backups(backups, identities);
    const auto decoded = acl::decode_payload(plaintext);
    require(decoded.size() == 1 && decoded[0].identity == opened.identity &&
            decoded[0].backup.path == opened.backup.path &&
            decoded[0].backup.descriptor == opened.backup.descriptor &&
            decoded[0].backup.denied_acl == opened.backup.denied_acl,
            "payload round trip changed a path, file identity, or saved ACL");
    const auto sealed = acl::protect_state(plaintext);
    require(acl::unprotect_state(std::span(sealed).subspan(acl::state_magic.size())) == plaintext,
            "DPAPI plaintext round trip changed the payload");
    require(acl::decode_backups(sealed)[0].backup.descriptor == opened.backup.descriptor,
            "sealed backup round trip changed original permissions");
    auto damaged = sealed;
    damaged.back() ^= std::byte{0x80};
    rejects([&] { static_cast<void>(acl::decode_backups(damaged)); }, "corrupted DPAPI state was accepted");
    damaged = sealed;
    damaged[0] ^= std::byte{1};
    rejects([&] { static_cast<void>(acl::decode_backups(damaged)); }, "wrong state magic was accepted");
    rejects([&] { static_cast<void>(acl::decode_backups(acl::state_magic)); }, "empty sealed state was accepted");
    rejects([&] { static_cast<void>(acl::protect_state({})); }, "empty DPAPI plaintext was accepted");
    rejects([&] { static_cast<void>(acl::unprotect_state({})); }, "empty DPAPI state was accepted");
    const std::vector<std::byte> excessive_plaintext(static_cast<size_t>(acl::max_state) + 1U);
    const auto excessive_sealed = acl::protect_state(excessive_plaintext);
    rejects([&] {
        static_cast<void>(acl::unprotect_state(std::span(excessive_sealed).subspan(acl::state_magic.size())));
    }, "oversized decrypted recovery data was accepted");
    const std::vector<std::byte> excessive_ciphertext(static_cast<size_t>(acl::max_state_file) + 1U);
    rejects([&] { static_cast<void>(acl::protect_state(excessive_ciphertext)); },
            "oversized encrypted recovery data was produced");
    for (std::size_t length = 0; length < plaintext.size(); ++length) {
        rejects([&] { static_cast<void>(acl::decode_payload(std::span(plaintext).first(length))); },
                "truncated recovery payload was accepted");
    }
    const std::size_t units_position = acl::payload_magic.size() + sizeof(std::uint32_t);
    const std::size_t descriptor_position = units_position + sizeof(std::uint32_t) +
        opened.backup.path.native().size() * sizeof(std::uint16_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t);
    const std::size_t denied_position = descriptor_position + sizeof(std::uint32_t) + opened.backup.descriptor.size();
    const std::array field_corruptions{
        std::pair{acl::payload_magic.size(), std::uint32_t{0}},
        std::pair{acl::payload_magic.size(), std::uint32_t{3}},
        std::pair{units_position, std::uint32_t{0}},
        std::pair{units_position, std::uint32_t{32768}},
        std::pair{descriptor_position, std::uint32_t{0}},
        std::pair{descriptor_position, std::uint32_t{1048577}},
        std::pair{descriptor_position, std::uint32_t{1048576}},
        std::pair{denied_position, std::uint32_t{0}},
        std::pair{denied_position, std::uint32_t{65536}},
        std::pair{denied_position, std::uint32_t{65535}}};
    for (const auto& [position, value] : field_corruptions) {
        auto changed = plaintext;
        replace_object(changed, position, value);
        rejects([&] { static_cast<void>(acl::decode_payload(changed)); }, "invalid payload field was accepted");
    }
    auto invalid_path = plaintext;
    replace_object(invalid_path, units_position + sizeof(std::uint32_t), std::uint16_t{0});
    rejects([&] { static_cast<void>(acl::decode_payload(invalid_path)); }, "embedded NUL path was accepted");
    auto trailing = plaintext;
    trailing.push_back(std::byte{0});
    rejects([&] { static_cast<void>(acl::decode_payload(trailing)); }, "payload trailing bytes were accepted");
    auto bad_magic = plaintext;
    bad_magic[0] ^= std::byte{1};
    rejects([&] { static_cast<void>(acl::decode_payload(bad_magic)); }, "wrong payload magic was accepted");
    rejects<std::logic_error>([&] { static_cast<void>(acl::encode_backups(backups, {})); },
                              "backup and identity count mismatch was accepted");
    rejects<std::logic_error>([&] { acl::restore_all(backups, {}); },
                              "backup and handle count mismatch was accepted");
    auto invalid_backup = backups;
    invalid_backup[0].denied_acl.clear();
    rejects([&] { static_cast<void>(acl::encode_backups(invalid_backup, identities)); },
            "backup without its protected ACL was serialized");
    invalid_backup = backups;
    invalid_backup[0].path = fs::path(std::wstring{L'C', L':', L'\\', wchar_t{0}, L'x'});
    rejects([&] { static_cast<void>(acl::encode_backups(invalid_backup, identities)); },
            "backup containing an embedded NUL path was serialized");
    invalid_backup = backups;
    invalid_backup[0].descriptor.resize(static_cast<size_t>(acl::max_state) + 1U);
    auto user = acl::current_user();
    rejects([&] { acl::write_state(fixture.state, invalid_backup, identities, user.sid()); },
            "oversized backup plaintext was persisted");
    require(!fs::exists(fixture.state), "oversized plaintext created a journal");
}

void protection_lifecycle_tests() {
    Fixture fixture;
    auto first = prepared_backup(fixture.renderer);
    auto second = prepared_backup(fixture.renderer32);
    auto user = acl::current_user();
    require(!acl::has_current_user_execute_deny(first.backup, user.sid()), "new fixture already denies execute");
    require(acl::restoration_state(first.backup, first.handle.get()) == acl::RestorationState::restored,
            "new fixture is not in its original permission state");
    require(!acl::restore_one(first.backup, first.handle.get()), "restoring original permissions reported a change");
    require(acl::open_target(fixture.renderer, FILE_READ_ATTRIBUTES).identity == first.identity,
            "independent opens disagree about file identity");
    acl::require_current_user_owner(first.handle.get(), user.sid());
    alignas(DWORD) std::array<std::byte, SECURITY_MAX_SID_SIZE> system{};
    DWORD system_length = static_cast<DWORD>(system.size());
    native_require(CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &system_length), "create SYSTEM SID");
    if (!EqualSid(user.sid(), system.data())) {
        rejects([&] { acl::require_current_user_owner(first.handle.get(), system.data()); },
                "wrong owner was accepted");
    }
    const std::array targets{fixture.renderer, fixture.renderer32};
    {
        auto protection = acl::Protection::apply(targets, fixture.state, fixture.log);
        require(fs::exists(fixture.state), "active protection has no recovery state");
        require(acl::read_state(fixture.state).size() == 2, "both renderer backups were not persisted");
        acl::verify_denied(first.backup, first.handle.get());
        acl::verify_denied(second.backup, second.handle.get());
        protection.restore(fixture.log);
        protection.restore(fixture.log);
        acl::verify_restored(first.backup, first.handle.get());
        acl::verify_restored(second.backup, second.handle.get());
        require(!fs::exists(fixture.state), "explicit restoration retained its journal");
    }
    {
        auto protection = acl::Protection::apply(std::span(targets).first(1), fixture.state, fixture.log);
        acl::verify_protection(first.backup, first.handle.get());
    }
    acl::verify_restored(first.backup, first.handle.get());
    require(!fs::exists(fixture.state), "destructor restoration retained its journal");
    acl::deny(first.backup, first.handle.get());
    rejects<steamfix::AppError>([&] {
        static_cast<void>(acl::Protection::apply(std::span(targets).first(1), fixture.state, fixture.log));
    }, "preexisting execute denial was backed up without a matching journal");
    require(!fs::exists(fixture.state), "preexisting denial created a misleading recovery backup");
    force_original(first);
    rejects<steamfix::AppError>([&] {
        static_cast<void>(acl::Protection::apply({}, fixture.state, fixture.log));
    }, "empty protection target set was accepted");
    const std::array excessive_targets{fixture.renderer, fixture.renderer32, fixture.renderer};
    rejects<steamfix::AppError>([&] {
        static_cast<void>(acl::Protection::apply(excessive_targets, fixture.state, fixture.log));
    }, "more than two renderer targets were accepted");
    const std::array duplicates{fixture.renderer, fixture.renderer};
    rejects<steamfix::AppError>([&] {
        static_cast<void>(acl::Protection::apply(duplicates, fixture.state, fixture.log));
    }, "duplicate renderer targets were accepted");
    const std::array duplicate_alias{fixture.renderer,
        fixture.renderer.parent_path() / L"." / fixture.renderer.filename()};
    rejects<steamfix::AppError>([&] {
        static_cast<void>(acl::Protection::apply(duplicate_alias, fixture.state, fixture.log));
    }, "two aliases of the same renderer were accepted");
    require(!fs::exists(fixture.state), "duplicate targets persisted an unusable journal");
    acl::verify_restored(first.backup, first.handle.get());
    acl::verify_restored(second.backup, second.handle.get());

    // Failure for one target must still attempt restoration of the others.
    acl::deny(second.backup, second.handle.get());
    const std::array saved{first.backup, second.backup};
    const std::array<UniqueHandle, 2> handles{
        UniqueHandle{}, acl::open_target(fixture.renderer32, READ_CONTROL | WRITE_DAC).handle};
    rejects([&] { acl::restore_all(saved, handles); }, "failed restoration handle was ignored");
    acl::verify_restored(second.backup, second.handle.get());
}

void pending_recovery_tests() {
    Fixture fixture;
    auto opened = prepared_backup(fixture.renderer);
    auto user = acl::current_user();
    const std::array backups{opened.backup};
    const std::array identities{opened.identity};
    require(!acl::recover_pending(fixture.state, fixture.log), "missing journal reported recovered work");
    acl::write_state(fixture.state, backups, identities, user.sid());
    acl::deny(opened.backup, opened.handle.get());
    require(acl::recover_pending(fixture.state, fixture.log), "interrupted session was not recovered");
    acl::verify_restored(opened.backup, opened.handle.get());
    require(!fs::exists(fixture.state), "successful recovery kept the journal");

    // Resume uses the persisted original ACL, including when protection is already active.
    acl::write_state(fixture.state, backups, identities, user.sid());
    acl::deny(opened.backup, opened.handle.get());
    {
        const std::array targets{fixture.renderer};
        auto resumed = acl::Protection::apply(targets, fixture.state, fixture.log);
        require(acl::read_state(fixture.state)[0].backup.descriptor == opened.backup.descriptor,
                "resuming replaced the original ACL backup");
        acl::verify_denied(opened.backup, opened.handle.get());
        resumed.restore(fixture.log);
    }
    acl::write_state(fixture.state, backups, identities, user.sid());
    require(acl::recover_pending(fixture.state, fixture.log), "already-restored pending journal was not cleaned");

    acl::write_state(fixture.state, backups, identities, user.sid());
    {
        const std::array targets{fixture.renderer};
        auto resumed = acl::Protection::apply(targets, fixture.state, fixture.log);
        acl::verify_denied(opened.backup, opened.handle.get());
        resumed.restore(fixture.log);
    }

    fs::path temporary = fixture.state;
    temporary += L".tmp";
    create_file(temporary);
    require(!acl::recover_pending(fixture.state, fixture.log) && !fs::exists(temporary),
            "incomplete temporary journal was not removed");

    auto wrong_identity = identities;
    wrong_identity[0].file_index ^= 1U;
    acl::write_state(fixture.state, backups, wrong_identity, user.sid());
    rejects<steamfix::AppError>([&] { static_cast<void>(acl::recover_pending(fixture.state, fixture.log)); },
                               "changed file identity was recovered");
    require(fs::exists(fixture.state), "identity mismatch discarded the original journal");
    native_require(DeleteFileW(fixture.state.c_str()), "remove mismatched test journal");

    // An unrelated ACL change must preserve both that ACL and the original journal.
    acl::write_state(fixture.state, backups, identities, user.sid());
    const auto saved_journal = file_bytes(fixture.state);
    const DWORD status = SetSecurityInfo(opened.handle.get(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr);
    if (status != ERROR_SUCCESS) acl::throw_win32(status);
    rejects([&] { static_cast<void>(acl::restoration_state(opened.backup, opened.handle.get())); },
            "external ACL change was mistaken for a saved state");
    rejects<steamfix::AppError>([&] { static_cast<void>(acl::recover_pending(fixture.state, fixture.log)); },
                               "external ACL change was overwritten by recovery");
    require(fs::exists(fixture.state), "external ACL change discarded the journal");
    const auto after_rejection = acl::current_descriptor(opened.handle.get());
    require(acl::descriptor_parts(after_rejection).dacl == nullptr,
            "recovery changed the external DACL before reporting failure");
    require(file_bytes(fixture.state) == saved_journal, "failed recovery modified the saved journal");
    force_original(opened);
    require(acl::recover_pending(fixture.state, fixture.log), "restored external change did not permit recovery");

    acl::write_state(fixture.state, backups, identities, user.sid());
    const std::array other_target{fixture.renderer32};
    rejects<steamfix::AppError>([&] {
        static_cast<void>(acl::Protection::apply(other_target, fixture.state, fixture.log));
    }, "mismatched saved target set was accepted");
    require(fs::exists(fixture.state), "saved target mismatch discarded the journal");
    native_require(DeleteFileW(fixture.state.c_str()), "remove saved target test journal");

    acl::write_state(fixture.state, backups, identities, user.sid());
    const std::array both_targets{fixture.renderer, fixture.renderer32};
    rejects<steamfix::AppError>([&] {
        static_cast<void>(acl::Protection::apply(both_targets, fixture.state, fixture.log));
    }, "saved and selected renderer count mismatch was accepted");
    native_require(DeleteFileW(fixture.state.c_str()), "remove saved count test journal");
    const std::array duplicate_backups{opened.backup, opened.backup};
    const std::array duplicate_identities{opened.identity, opened.identity};
    acl::write_state(fixture.state, duplicate_backups, duplicate_identities, user.sid());
    rejects<steamfix::AppError>([&] {
        static_cast<void>(acl::Protection::apply(both_targets, fixture.state, fixture.log));
    }, "duplicate persisted renderer paths were accepted");
    require(fs::exists(fixture.state), "invalid saved renderer set discarded the journal");
    native_require(DeleteFileW(fixture.state.c_str()), "remove duplicate saved test journal");

    {
        const std::array targets{fixture.renderer};
        auto protection = acl::Protection::apply(targets, fixture.state, fixture.log);
        const DWORD changed = SetSecurityInfo(opened.handle.get(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr);
        if (changed != ERROR_SUCCESS) acl::throw_win32(changed);
    }
    require(fs::exists(fixture.state), "failed destructor restoration discarded the journal");
    const auto external = acl::current_descriptor(opened.handle.get());
    require(acl::descriptor_parts(external).dacl == nullptr, "destructor overwrote an external permission change");
    force_original(opened);
    require(acl::recover_pending(fixture.state, fixture.log), "preserved destructor-failure journal was unusable");

    opened.handle.reset();
    native_require(DeleteFileW(fixture.renderer.c_str()), "remove test renderer");
    const acl::RecoveryEntry missing{opened.backup, opened.identity};
    require(!acl::prepare_recovery_entry(missing, fixture.log), "missing renderer passed recovery preflight");
    acl::write_state(fixture.state, backups, identities, user.sid());
    rejects<steamfix::AppError>([&] { static_cast<void>(acl::recover_pending(fixture.state, fixture.log)); },
                               "missing renderer discarded its pending recovery");
    require(fs::exists(fixture.state), "missing renderer discarded its journal");
}

void state_file_and_neighbor_tests() {
    Fixture fixture;
    auto opened = prepared_backup(fixture.renderer);
    auto user = acl::current_user();
    const std::array backups{opened.backup};
    const std::array identities{opened.identity};
    const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}};
    create_file(fixture.state, bytes);
    rejects([&] { static_cast<void>(acl::read_state(fixture.state)); }, "invalid journal bytes were accepted");
    // A failed commit must close and remove its exclusively opened .tmp file.
    rejects([&] { acl::write_state(fixture.state, backups, identities, user.sid()); },
            "journal writer replaced an existing backup");
    require(file_bytes(fixture.state) == std::vector<std::byte>(bytes.begin(), bytes.end()),
            "failed journal commit modified the existing destination");
    fs::path temporary = fixture.state;
    temporary += L".tmp";
    require(!fs::exists(temporary), "failed journal commit leaked a locked temporary file");
    native_require(DeleteFileW(fixture.state.c_str()), "remove invalid test journal");
    create_file(fixture.state);
    rejects([&] { static_cast<void>(acl::read_state(fixture.state)); }, "empty journal file was accepted");
    {
        const UniqueHandle file(CreateFileW(fixture.state.c_str(), GENERIC_WRITE, 0, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        LARGE_INTEGER size{};
        size.QuadPart = static_cast<LONGLONG>(acl::max_state_file) + 1;
        native_require(SetFilePointerEx(file.get(), size, nullptr, FILE_BEGIN), "size test journal");
        native_require(SetEndOfFile(file.get()), "extend test journal");
    }
    rejects([&] { static_cast<void>(acl::read_state(fixture.state)); }, "oversized journal file was accepted");
    native_require(DeleteFileW(fixture.state.c_str()), "remove oversized test journal");
    const fs::path steam = fixture.directory.path() / L"steam.exe";
    native_require(DeleteFileW(steam.c_str()), "remove test steam neighbor");
    require(!acl::has_regular_steam_neighbor(fixture.renderer), "missing Steam neighbor was accepted");
    rejects([&] { acl::require_regular_steam_neighbor(fixture.renderer); }, "missing Steam neighbor was required");
    native_require(CreateDirectoryW(steam.c_str(), nullptr), "create directory Steam neighbor");
    require(!acl::has_regular_steam_neighbor(fixture.renderer), "directory Steam neighbor was accepted");
    const acl::RecoveryEntry entry{opened.backup, opened.identity};
    require(!acl::prepare_recovery_entry(entry, fixture.log), "unsafe Steam neighbor passed preflight");
}

void journal_write_failure_tests() {
    for (const bool fail_flush : {false, true}) {
        Fixture fixture;
        const auto first = prepared_backup(fixture.renderer);
        const auto second = prepared_backup(fixture.renderer32);
        const auto first_bytes = file_bytes(fixture.renderer);
        const auto second_bytes = file_bytes(fixture.renderer32);
        const std::array targets{fixture.renderer, fixture.renderer32};
        acl_fault::Plan plan;
        if (fail_flush) plan.flush_failures_remaining = 1;
        else plan.write_failures_remaining = 1;
        {
            const acl_fault::Scope faults(plan);
            rejects<steamfix::AppError>([&] {
                try {
                    static_cast<void>(acl::Protection::apply(targets, fixture.state, fixture.log));
                } catch (const steamfix::AppError& error) {
                    require(error.stage() == "ACL recovery persistence",
                            "journal fault failed outside persistence");
                    throw;
                }
            }, "journal write/flush fault did not stop protection");
        }
        require(plan.write_failures == (fail_flush ? 0 : 1) &&
                plan.flush_failures == (fail_flush ? 1 : 0) && plan.security_calls == 0,
                "journal fault was not consumed before all renderer permission changes");
        require(plan.write_failures_remaining == 0 && plan.flush_failures_remaining == 0,
                "journal fault remained armed");
        fs::path temporary = fixture.state;
        temporary += L".tmp";
        require(!fs::exists(fixture.state) && !fs::exists(temporary),
                "failed journal I/O left an exclusively locked temporary file or committed journal");
        acl::verify_restored(first.backup, first.handle.get());
        acl::verify_restored(second.backup, second.handle.get());
        require(file_bytes(fixture.renderer) == first_bytes && file_bytes(fixture.renderer32) == second_bytes,
                "failed journal I/O changed renderer contents");

        // A successful retry also proves cleanup released the old .tmp name and handle.
        auto retried = acl::Protection::apply(targets, fixture.state, fixture.log);
        acl::verify_denied(first.backup, first.handle.get());
        acl::verify_denied(second.backup, second.handle.get());
        retried.restore(fixture.log);
        acl::verify_restored(first.backup, first.handle.get());
        acl::verify_restored(second.backup, second.handle.get());
        require(!fs::exists(fixture.state) && !fs::exists(temporary),
                "retry after failed journal I/O retained recovery files");
    }
}

void partial_denial_rollback_tests() {
    for (const bool fail_rollback : {false, true}) {
        Fixture fixture;
        const auto first = prepared_backup(fixture.renderer);
        const auto second = prepared_backup(fixture.renderer32);
        const auto first_bytes = file_bytes(fixture.renderer);
        const auto second_bytes = file_bytes(fixture.renderer32);
        const std::array targets{fixture.renderer, fixture.renderer32};
        acl_fault::Plan plan;
        // In a fresh apply, calls 1 and 2 deny the first and second renderer.
        // Call 3 restores the already-denied first renderer during rollback.
        plan.security_failure_calls = {2, fail_rollback ? 3 : 0};
        {
            const acl_fault::Scope faults(plan);
            rejects<steamfix::AppError>([&] {
                try {
                    static_cast<void>(acl::Protection::apply(targets, fixture.state, fixture.log));
                } catch (const steamfix::AppError& error) {
                    require(error.stage() == "renderer permission denial",
                            "second denial fault failed outside denial rollback");
                    throw;
                }
            }, "second renderer denial fault did not fail protection");
        }
        require(plan.security_calls == 3 && plan.security_failures == (fail_rollback ? 2 : 1),
                "second denial and first rollback did not reach the intended finite faults");
        acl::verify_restored(second.backup, second.handle.get());
        fs::path temporary = fixture.state;
        temporary += L".tmp";
        require(!fs::exists(temporary), "denial rollback retained a temporary journal");
        if (fail_rollback) {
            acl::verify_denied(first.backup, first.handle.get());
            require(fs::exists(fixture.state), "failed rollback discarded the original recovery journal");
            const auto saved = acl::read_state(fixture.state);
            require(saved.size() == 2 && saved[0].backup.descriptor == first.backup.descriptor &&
                    saved[1].backup.descriptor == second.backup.descriptor &&
                    saved[0].identity == first.identity && saved[1].identity == second.identity,
                    "failed rollback did not preserve both original ACLs and identities");
            require(acl::recover_pending(fixture.state, fixture.log),
                    "real recovery could not finish the interrupted rollback");
        } else {
            acl::verify_restored(first.backup, first.handle.get());
            require(!fs::exists(fixture.state), "successful denial rollback retained its journal");
        }
        acl::verify_restored(first.backup, first.handle.get());
        acl::verify_restored(second.backup, second.handle.get());
        require(!fs::exists(fixture.state) && file_bytes(fixture.renderer) == first_bytes &&
                file_bytes(fixture.renderer32) == second_bytes,
                "rollback/recovery left a journal or changed renderer contents");
    }
}

} // namespace

int main() {
    try {
        binary_and_utf16_tests();
        acl_and_descriptor_tests();
        serialization_tests();
        protection_lifecycle_tests();
        pending_recovery_tests();
        state_file_and_neighbor_tests();
        journal_write_failure_tests();
        partial_denial_rollback_tests();
        std::cout << "ACL tests passed: " << assertions << " assertions using real Win32 APIs.\n";
    } catch (const std::exception& error) {
        std::cerr << "ACL regression test failed after " << assertions << " assertions: "
                  << error.what() << '\n';
        return 1;
    }
    return 0;
}
