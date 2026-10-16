#include "steamfix/steam.hpp"

#include "steamfix/paths.hpp"
#include "steamfix/process.hpp"
#include "steamfix/win32.hpp"

#include <windows.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace steamfix::steam {
namespace {

class RegistryKey {
public:
    explicit RegistryKey(HKEY key) noexcept : key_(key) {}
    ~RegistryKey() noexcept { if (key_ != nullptr) RegCloseKey(key_); }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    RegistryKey(RegistryKey&&) = delete;
    RegistryKey& operator=(RegistryKey&&) = delete;
    [[nodiscard]] HKEY get() const noexcept { return key_; }
private:
    HKEY key_;
};

std::wstring registry_string(const wchar_t* name) {
    HKEY raw = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &raw);
    if (status != ERROR_SUCCESS) throw std::system_error(static_cast<int>(status), std::system_category());
    const RegistryKey key(raw);
    constexpr DWORD allowed_types = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    constexpr DWORD max_registry_bytes = 128U * 1024U;
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD type = 0;
        DWORD bytes = 0;
        status = RegGetValueW(key.get(), nullptr, name, allowed_types, &type, nullptr, &bytes);
        if (status != ERROR_SUCCESS) {
            throw std::system_error(static_cast<int>(status), std::system_category());
        }
        if (bytes == 0 || bytes > max_registry_bytes || bytes % sizeof(wchar_t) != 0) {
            throw std::runtime_error("Steam registry value has an invalid string size");
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(bytes / sizeof(wchar_t)) + 1U, L'\0');
        status = RegGetValueW(key.get(), nullptr, name, allowed_types, &type, buffer.data(), &bytes);
        if (status == ERROR_MORE_DATA) continue;
        if (status != ERROR_SUCCESS) {
            throw std::system_error(static_cast<int>(status), std::system_category());
        }
        if (bytes == 0 || bytes > max_registry_bytes || bytes % sizeof(wchar_t) != 0) {
            throw std::runtime_error("Steam registry value has an invalid string size");
        }
        if (bytes > buffer.size() * sizeof(wchar_t)) {
            throw std::runtime_error("Steam registry value returned an inconsistent string size");
        }
        std::wstring_view value(buffer.data(), bytes / sizeof(wchar_t));
        while (!value.empty() && value.back() == L'\0') value.remove_suffix(1);
        if (value.empty() || value.find(L'\0') != std::wstring_view::npos) {
            throw std::runtime_error("Steam registry value is empty or contains an embedded NUL");
        }
        return std::wstring(value);
    }
    throw std::runtime_error("Steam registry value changed repeatedly while it was being read");
}

fs::path verify(const fs::path& candidate) {
    const fs::path directory = paths::name_is(candidate, L"steam.exe") ? candidate.parent_path() : candidate;
    if (!fs::is_regular_file(directory / L"steam.exe")) {
        throw fs::filesystem_error("steam.exe not found in discovered directory", directory,
                                   std::make_error_code(std::errc::no_such_file_or_directory));
    }
    return fs::canonical(directory);
}

} // namespace

fs::path discover() {
    std::vector<std::string> failures;
    try {
        for (const DWORD pid : process::parent_chain()) {
            try {
                const UniqueHandle handle = process::open_query(pid);
                const fs::path candidate = process::image_path(handle.get());
                if (paths::name_is(candidate, L"steam.exe")) return verify(candidate);
            } catch (const std::exception&) {}
        }
    } catch (const std::exception& error) {
        failures.push_back("Steam ancestor inspection: " + std::string(error.what()));
    }

    try {
        return verify(registry_string(L"SteamExe"));
    } catch (const std::exception& error) {
        failures.push_back("SteamExe registry value: " + std::string(error.what()));
    }
    try {
        return verify(registry_string(L"SteamPath"));
    } catch (const std::exception& error) {
        failures.push_back("SteamPath registry value: " + std::string(error.what()));
    }
    std::ostringstream message;
    message << "all Steam discovery methods failed";
    for (const std::string& failure : failures) message << "; " << failure;
    throw std::runtime_error(message.str());
}

} // namespace steamfix::steam
