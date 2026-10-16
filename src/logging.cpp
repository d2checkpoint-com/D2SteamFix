#include "steamfix/logging.hpp"

#include "steamfix/elevation.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace steamfix {
namespace {
constexpr std::uintmax_t max_log_size = 4U * 1024U * 1024U;

void rotate(const fs::path& path) noexcept {
    std::error_code error;
    if (fs::file_size(path, error) <= max_log_size || error) return;
    const fs::path old = path.parent_path() / (path.stem().native() + L".log.old");
    fs::remove(old, error);
    error.clear();
    fs::rename(path, old, error);
}
} // namespace

Logger::Logger(const fs::path& path) {
    rotate(path);
    file_ = elevation::open_file(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file_.get(), &information)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "inspect log");
    }
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        information.nNumberOfLinks != 1) {
        throw std::runtime_error("steamfix.log must be a regular file without links");
    }
}

void Logger::info(const std::string_view message) const noexcept { write("INFO", message); }
void Logger::warn(const std::string_view message) const noexcept { write("WARN", message); }
void Logger::error(const std::string_view message) const noexcept { write("ERROR", message); }

void Logger::write(const std::string_view severity, const std::string_view message) const noexcept {
    try {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        std::lock_guard lock(mutex_);
        std::ostringstream output;
        size_t start = 0;
        do {
            const size_t end = message.find('\n', start);
            const std::string_view line =
                message.substr(start, end == std::string_view::npos ? end : end - start);
            output << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2)
                  << now.wMonth << '-' << std::setw(2) << now.wDay << 'T' << std::setw(2)
                  << now.wHour << ':' << std::setw(2) << now.wMinute << ':' << std::setw(2)
                  << now.wSecond << '.' << std::setw(3) << now.wMilliseconds << "Z [" << severity
                  << "] [pid=" << GetCurrentProcessId() << "] [v" << STEAMFIX_VERSION << "] "
                  << line << '\n';
            if (end == std::string_view::npos) break;
            start = end + 1;
        } while (start <= message.size());
        const std::string data = output.str();
        size_t offset = 0;
        while (offset < data.size()) {
            const DWORD requested = static_cast<DWORD>(std::min<size_t>(data.size() - offset, MAXDWORD));
            DWORD written = 0;
            if (!WriteFile(file_.get(), data.data() + offset, requested, &written, nullptr) || written == 0) {
                OutputDebugStringW(L"D2SteamFix: writing steamfix.log failed.\n");
                return;
            }
            offset += written;
        }
    } catch (...) {
        OutputDebugStringW(L"D2SteamFix: writing steamfix.log failed.\n");
    }
}

} // namespace steamfix
