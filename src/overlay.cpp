#include "steamfix/overlay.hpp"
#include "steamfix/win32.hpp"

#include <tlhelp32.h>

#include <cwchar>
#include <system_error>

namespace steamfix::overlay {
namespace {

constexpr int max_snapshot_attempts = 8;

UniqueHandle module_snapshot() {
    DWORD error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < max_snapshot_attempts; ++attempt) {
        const HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                              GetCurrentProcessId());
        if (raw != INVALID_HANDLE_VALUE) return UniqueHandle(raw);
        error = GetLastError();
        if (error != ERROR_BAD_LENGTH) break;
        if (attempt + 1 < max_snapshot_attempts) Sleep(1);
    }
    throw std::system_error(static_cast<int>(error), std::system_category());
}

} // namespace

std::optional<std::filesystem::path> loaded_renderer() {
    const UniqueHandle snapshot = module_snapshot();
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    do {
        if (_wcsicmp(entry.szModule, L"GameOverlayRenderer64.dll") == 0 ||
            _wcsicmp(entry.szModule, L"GameOverlayRenderer.dll") == 0) {
            return std::filesystem::path(entry.szExePath);
        }
    } while (Module32NextW(snapshot.get(), &entry));
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) {
        throw std::system_error(static_cast<int>(error), std::system_category());
    }
    return std::nullopt;
}

} // namespace steamfix::overlay
