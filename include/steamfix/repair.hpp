#ifndef STEAMFIX_REPAIR_HPP
#define STEAMFIX_REPAIR_HPP

#include <filesystem>

namespace steamfix {
class Logger;
}

namespace steamfix::repair {

// Check effective execute access before accepting permissions as a new baseline.
bool needed(const std::filesystem::path& steam_directory, const Logger& log);

// Cancellation leaves applications, renderer files, and recovery state untouched.
bool run(const std::filesystem::path& game_directory,
         const std::filesystem::path& recovery_path, const Logger& log);

} // namespace steamfix::repair

#endif // STEAMFIX_REPAIR_HPP
