#include "steamfix/paths.hpp"

#include <windows.h>

#include <climits>
#include <cstddef>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace steamfix::paths {

bool equal_windows(const fs::path& left, const fs::path& right) {
    std::error_code equivalent_error;
    if (fs::equivalent(left, right, equivalent_error)) return true;

    const std::wstring& a = left.native();
    const std::wstring& b = right.native();
    if (a.size() > static_cast<std::size_t>(INT_MAX) || b.size() > static_cast<std::size_t>(INT_MAX)) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool name_is(const fs::path& path, const std::wstring_view expected) {
    const std::wstring name = path.filename().native();
    if (name.size() != expected.size() || name.size() > static_cast<std::size_t>(INT_MAX)) return false;
    return CompareStringOrdinal(name.data(), static_cast<int>(name.size()), expected.data(),
                                static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

} // namespace steamfix::paths
