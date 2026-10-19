#pragma once

#include <filesystem>

namespace paths {

// <game Bin>\SnowMinimap, created on first use. Holds the config, log and map images.
const std::filesystem::path& ModDir();
std::filesystem::path MapsDir();

void Init(void* dllModule);

} // namespace paths
