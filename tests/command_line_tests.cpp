#include "steamfix/command_line.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <span>
#endif

namespace {

struct QuotingCase {
    std::wstring_view argument;
    std::wstring_view expected;
};

constexpr std::array cases{
    QuotingCase{L"plain", L"plain"},
    QuotingCase{L"", LR"("")"},
    QuotingCase{L"two words", LR"("two words")"},
    QuotingCase{L"a\tb", L"\"a\tb\""},
    QuotingCase{L"line\nbreak", L"line\nbreak"},
    QuotingCase{LR"(say"hi)", LR"("say\"hi")"},
    QuotingCase{LR"(C:\plain\)", LR"(C:\plain\)"},
    QuotingCase{LR"(C:\two words\)", LR"("C:\two words\\")"},
    QuotingCase{LR"(a\"b)", LR"("a\\\"b")"},
    QuotingCase{LR"(a\\"b)", LR"("a\\\\\"b")"},
    QuotingCase{L"修復 🎮", L"\"修復 🎮\""},
};

bool check_expected_quoting() {
    for (const auto& test : cases) {
        std::wstring line;
        steamfix::command_line::append_quoted(line, test.argument);
        if (line != test.expected) {
            std::wcerr << L"Unexpected quoting for argument: " << test.argument << L'\n';
            return false;
        }
    }

    std::wstring line{L"steamfix.exe"};
    steamfix::command_line::append_quoted(line, L"");
    steamfix::command_line::append_quoted(line, L"two words");
    if (line != LR"(steamfix.exe "" "two words")") {
        std::wcerr << L"Appending arguments did not preserve separators.\n";
        return false;
    }
    return true;
}

#ifdef _WIN32
struct CommandLineDeleter {
    void operator()(wchar_t** arguments) const noexcept {
        LocalFree(arguments);
    }
};

bool check_windows_round_trip() {
    std::wstring line{L"steamfix.exe"};
    for (const auto& test : cases) {
        steamfix::command_line::append_quoted(line, test.argument);
    }

    int count{};
    const std::unique_ptr<wchar_t*, CommandLineDeleter> arguments{
        CommandLineToArgvW(line.c_str(), &count)};
    if (!arguments || count <= 0 || static_cast<std::size_t>(count) != cases.size() + 1U) {
        std::wcerr << L"Windows could not parse the generated command line.\n";
        return false;
    }
    const std::span parsed{arguments.get(), static_cast<std::size_t>(count)};
    auto argument = parsed.subspan(1).begin();
    for (const auto& test : cases) {
        if (std::wstring_view{*argument} != test.argument) {
            std::wcerr << L"Windows round trip changed an argument: " << test.argument << L'\n';
            return false;
        }
        ++argument;
    }
    return true;
}
#endif

} // namespace

int main() {
    if (!check_expected_quoting()) return 1;
#ifdef _WIN32
    if (!check_windows_round_trip()) return 1;
#endif
    return 0;
}
