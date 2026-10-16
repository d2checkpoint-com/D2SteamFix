#include "steamfix/command_line.hpp"

#include <cstddef>

namespace steamfix::command_line {

void append_quoted(std::wstring& line, const std::wstring_view argument) {
    if (!line.empty()) line.push_back(L' ');
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        line.append(argument);
        return;
    }
    line.push_back(L'\"');
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'\"') {
            line.append(slashes * 2U + 1U, L'\\');
            line.push_back(L'\"');
            slashes = 0;
        } else {
            line.append(slashes, L'\\');
            slashes = 0;
            line.push_back(character);
        }
    }
    line.append(slashes * 2U, L'\\');
    line.push_back(L'\"');
}

} // namespace steamfix::command_line
