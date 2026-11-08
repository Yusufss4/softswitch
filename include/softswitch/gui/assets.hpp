#pragma once

/// @file
/// The GUI's static files, embedded into softswitchd at build time (cmake/EmbedAssets.cmake).

#include <cstdint>
#include <span>
#include <string_view>

namespace softswitch::gui {

struct Asset {
  std::string_view path;  ///< e.g. "/app.js"
  std::string_view content_type;
  std::span<const std::uint8_t> data;
};

/// The asset served at @p path ("/" means "/index.html"), or nullptr.
[[nodiscard]] const Asset* find_asset(std::string_view path) noexcept;

}  // namespace softswitch::gui
