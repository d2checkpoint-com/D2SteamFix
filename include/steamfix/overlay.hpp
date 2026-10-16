#ifndef STEAMFIX_OVERLAY_HPP
#define STEAMFIX_OVERLAY_HPP

#include <filesystem>
#include <optional>

namespace steamfix::overlay {
std::optional<std::filesystem::path> loaded_renderer();
}

#endif // STEAMFIX_OVERLAY_HPP
