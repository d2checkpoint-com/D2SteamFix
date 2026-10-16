#ifndef STEAMFIX_COMMAND_LINE_HPP
#define STEAMFIX_COMMAND_LINE_HPP

#include <string>
#include <string_view>

namespace steamfix::command_line {

void append_quoted(std::wstring& line, std::wstring_view argument);

} // namespace steamfix::command_line

#endif // STEAMFIX_COMMAND_LINE_HPP
