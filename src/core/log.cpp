#include "core/log.h"

#include "core/paths.h"

#include <windows.h>

#include <fstream>
#include <mutex>

namespace logger {

static std::mutex g_mutex;
static std::ofstream g_file;

void Init()
{
    std::lock_guard lock(g_mutex);
    g_file.open(paths::ModDir() / L"SnowMinimap.log", std::ios::out | std::ios::trunc);
}

void Write(std::string_view level, std::string_view message)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::string line = std::format("[{:02}:{:02}:{:02}.{:03}] {} {}\n", t.wHour, t.wMinute, t.wSecond,
                                         t.wMilliseconds, level, message);
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
    OutputDebugStringA(line.c_str());
}

} // namespace logger
