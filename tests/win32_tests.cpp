#include "steamfix/win32.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

static_assert(!std::is_copy_constructible_v<steamfix::UniqueHandle>);
static_assert(!std::is_copy_assignable_v<steamfix::UniqueHandle>);
static_assert(std::is_nothrow_move_constructible_v<steamfix::UniqueHandle>);
static_assert(std::is_nothrow_move_assignable_v<steamfix::UniqueHandle>);
static_assert(std::is_nothrow_destructible_v<steamfix::UniqueHandle>);
static_assert(!std::is_copy_constructible_v<steamfix::LocalMemory>);
static_assert(!std::is_copy_assignable_v<steamfix::LocalMemory>);
static_assert(!std::is_move_constructible_v<steamfix::LocalMemory>);
static_assert(!std::is_move_assignable_v<steamfix::LocalMemory>);
static_assert(std::is_nothrow_destructible_v<steamfix::LocalMemory>);

bool is_open(HANDLE handle) noexcept {
    DWORD flags{};
    return GetHandleInformation(handle, &flags) != FALSE;
}

bool check_handle_ownership() {
    HANDLE observed{};
    {
        steamfix::UniqueHandle original{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        observed = original.get();
        steamfix::UniqueHandle moved{std::move(original)};
        if (original.get() != nullptr || moved.get() != observed || !is_open(observed)) {
            std::cerr << "Moving a handle did not transfer ownership.\n";
            return false;
        }

        moved.reset(moved.get());
        if (!is_open(observed)) {
            std::cerr << "Resetting to the current handle closed it.\n";
            return false;
        }

        steamfix::UniqueHandle assigned{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const HANDLE replaced = assigned.get();
        assigned = std::move(moved);
        if (moved.get() != nullptr || assigned.get() != observed || is_open(replaced)) {
            std::cerr << "Move assignment did not close the replaced handle.\n";
            return false;
        }
    }
    if (is_open(observed)) {
        std::cerr << "Destruction did not close the owned handle.\n";
        return false;
    }

    steamfix::UniqueHandle released{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    steamfix::UniqueHandle recipient{released.release()};
    if (released.get() != nullptr || !is_open(recipient.get())) {
        std::cerr << "Release did not leave the transferred handle open.\n";
        return false;
    }
    const HANDLE reset = recipient.get();
    recipient.reset();
    if (recipient.get() != nullptr || is_open(reset)) {
        std::cerr << "Reset did not close and clear the owned handle.\n";
        return false;
    }
    return true;
}

bool check_text_conversion() {
    constexpr std::wstring_view wide{L"修復 🎮\0tail", std::size(L"修復 🎮\0tail") - 1U};
    constexpr std::string_view utf8{"修復 🎮\0tail", std::size("修復 🎮\0tail") - 1U};
    if (steamfix::narrow(wide) != utf8 || steamfix::widen(utf8) != wide ||
        !steamfix::narrow(L"").empty() || !steamfix::widen("").empty()) {
        std::cerr << "Text conversion changed UTF-8, embedded NULs, or empty input.\n";
        return false;
    }
    return true;
}

bool check_exit_status() noexcept {
    return steamfix::windows_exit_status(DWORD{0}) == 0 &&
           steamfix::windows_exit_status(DWORD{259}) == 259 &&
           steamfix::windows_exit_status(DWORD{0xffffffffUL}) == -1 &&
           steamfix::windows_exit_status(DWORD{0x80000000UL}) == std::numeric_limits<int>::min();
}

} // namespace

int main() {
    try {
        if (!check_handle_ownership() || !check_text_conversion()) return 1;
        if (!check_exit_status()) {
            std::cerr << "Windows exit status lost its 32-bit representation.\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "Win32 helper regression test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
