#pragma once

#include "steamfix/win32.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace steamfix {
class Logger;
}

namespace steamfix::acl {

struct Backup {
    std::filesystem::path path;
    std::vector<std::byte> descriptor;
    std::vector<std::byte> denied_acl;
};

class Protection {
public:
    ~Protection() noexcept;
    Protection(const Protection&) = delete;
    Protection& operator=(const Protection&) = delete;
    Protection(Protection&&) = delete;
    Protection& operator=(Protection&&) = delete;

    static Protection apply(std::span<const std::filesystem::path> targets,
                            std::filesystem::path state_path, const Logger& log);
    void restore(const Logger& log);

private:
    enum class State {
        active,
        restored,
    };

    Protection(std::vector<Backup> backups, std::vector<UniqueHandle> handles,
               std::filesystem::path state_path) noexcept
        : backups_(std::move(backups)), handles_(std::move(handles)),
          state_path_(std::move(state_path)), state_(State::active) {}
    std::vector<Backup> backups_;
    std::vector<UniqueHandle> handles_;
    std::filesystem::path state_path_;
    State state_;
};

std::filesystem::path recovery_path(const std::filesystem::path& game_directory);
bool recover_pending(const std::filesystem::path& path, const Logger& log);

} // namespace steamfix::acl
