#ifndef STEAMFIX_UI_HPP
#define STEAMFIX_UI_HPP

#include <string_view>

namespace steamfix::ui {
void info(std::wstring_view message) noexcept;
void error(std::wstring_view message) noexcept;
}

#endif // STEAMFIX_UI_HPP
