#pragma once

#include "steamfix/win32.hpp"

#include <filesystem>
#include <mutex>
#include <string_view>

namespace steamfix {

class Logger {
public:
    explicit Logger(const std::filesystem::path& path);
    void info(std::string_view message) const noexcept;
    void warn(std::string_view message) const noexcept;
    void error(std::string_view message) const noexcept;

private:
    void write(std::string_view severity, std::string_view message) const noexcept;
    mutable std::mutex mutex_;
    UniqueHandle file_;
};

} // namespace steamfix
