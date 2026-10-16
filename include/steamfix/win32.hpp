#ifndef STEAMFIX_WIN32_HPP
#define STEAMFIX_WIN32_HPP

#include <windows.h>

#include <bit>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace steamfix {

// Windows process statuses are 32-bit bit patterns, including NTSTATUS values.
// Preserve those bits when crossing the signed C++ entry-point boundary.
inline int windows_exit_status(DWORD value) noexcept {
    static_assert(sizeof(DWORD) == sizeof(int));
    return std::bit_cast<int>(value);
}

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {
        if (value == nullptr || value == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
    }
    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE old = value_;
        value_ = nullptr;
        return old;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ == value) return;
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

class LocalMemory {
public:
    explicit LocalMemory(void* value = nullptr) noexcept : value_(value) {}
    ~LocalMemory() noexcept { if (value_ != nullptr) LocalFree(value_); }
    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;
    LocalMemory(LocalMemory&&) = delete;
    LocalMemory& operator=(LocalMemory&&) = delete;
private:
    void* value_;
};

inline std::string narrow(const std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("wide string is too large to convert");
    }
    int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    DWORD flags = WC_ERR_INVALID_CHARS;
    if (needed <= 0) {
        flags = 0;
        needed = WideCharToMultiByte(CP_UTF8, flags, value.data(), static_cast<int>(value.size()),
                                     nullptr, 0, nullptr, nullptr);
    }
    if (needed <= 0) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    std::string result(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, flags, value.data(), static_cast<int>(value.size()),
                            result.data(), needed, nullptr, nullptr) == 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    return result;
}

inline std::wstring widen(const std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("narrow string is too large to convert");
    }
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (needed <= 0) {
        code_page = CP_ACP;
        flags = 0;
        needed = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (needed <= 0) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()),
                            result.data(), needed) == 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    return result;
}

inline std::string display_path(const std::filesystem::path& value) {
    return narrow(value.native());
}

inline std::string windows_error(DWORD code = GetLastError()) {
    return std::system_error(windows_exit_status(code), std::system_category()).what();
}

} // namespace steamfix

#endif // STEAMFIX_WIN32_HPP
