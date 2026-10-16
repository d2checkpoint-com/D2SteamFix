#ifndef STEAMFIX_PATHS_HPP
#define STEAMFIX_PATHS_HPP

#include <filesystem>
#include <string_view>

namespace steamfix::paths {

bool equal_windows(const std::filesystem::path& left, const std::filesystem::path& right);
bool name_is(const std::filesystem::path& path, std::wstring_view expected);

} // namespace steamfix::paths

#endif // STEAMFIX_PATHS_HPP
