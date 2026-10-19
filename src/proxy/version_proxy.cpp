#include "proxy/version_proxy.h"

#include <windows.h>

#include <iterator>

// Order must match version_stubs.asm and version.def.
static constexpr const char* kExportNames[] = {
    "GetFileVersionInfoA",     "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",   "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",  "GetFileVersionInfoW",
    "VerFindFileA",            "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",         "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",          "VerQueryValueW",
};

extern "C" {
FARPROC g_versionProcs[std::size(kExportNames)];
}

namespace proxy {

bool LoadRealVersionDll()
{
    wchar_t path[MAX_PATH];
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len + 13 >= MAX_PATH)
        return false;
    wcscat_s(path, L"\\version.dll");

    HMODULE real = LoadLibraryW(path);
    if (!real)
        return false;

    for (size_t i = 0; i < std::size(kExportNames); ++i) {
        g_versionProcs[i] = GetProcAddress(real, kExportNames[i]);
        if (!g_versionProcs[i])
            return false;
    }
    return true;
}

} // namespace proxy
