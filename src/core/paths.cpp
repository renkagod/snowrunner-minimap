#include "core/paths.h"

#include <windows.h>

namespace paths {

static std::filesystem::path g_modDir;

void Init(void* dllModule)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(static_cast<HMODULE>(dllModule), buf, MAX_PATH);
    g_modDir = std::filesystem::path(buf).parent_path() / L"SnowMinimap";
    std::error_code ec;
    std::filesystem::create_directories(MapsDir(), ec);
}

const std::filesystem::path& ModDir()
{
    return g_modDir;
}

std::filesystem::path MapsDir()
{
    return g_modDir / L"maps";
}

} // namespace paths
