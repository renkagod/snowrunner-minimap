#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "dev/renderdoc.h"
#include "game/game_state.h"
#include "game/memory.h"
#include "hooks/present_hook.h"
#include "proxy/version_proxy.h"

#include <MinHook.h>
#include <windows.h>

#include <filesystem>

static HMODULE g_module = nullptr;

// Other tools in Sources\Bin (resource converter, crash reporter) load version.dll too.
static bool IsGameProcess()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return _wcsicmp(std::filesystem::path(exe).filename().c_str(), L"SnowRunner.exe") == 0;
}

static BOOL CALLBACK FindVisibleWindow(HWND hwnd, LPARAM found)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        *reinterpret_cast<HWND*>(found) = hwnd;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI ModThread(LPVOID)
{
    paths::Init(g_module);
    logger::Init();
    logger::Info("SnowMinimap loaded");
    config::Load();
    dev::LoadRenderDoc(config::Get().renderdocDll);

    if (!mem::InitGameImage()) {
        logger::Error("Could not parse SnowRunner.exe sections; mod disabled");
        return 0;
    }
    game::Start();

    if (MH_Initialize() != MH_OK) {
        logger::Error("MinHook init failed; overlay disabled");
        return 0;
    }

    // Once the game shows its window, d3d11 and dxgi are loaded and a throwaway device no longer
    // races the game's own startup.
    HWND window = nullptr;
    while (!window) {
        Sleep(250);
        EnumWindows(FindVisibleWindow, reinterpret_cast<LPARAM>(&window));
    }
    hooks::InstallPresentHook();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        if (!proxy::LoadRealVersionDll())
            return FALSE;
        if (IsGameProcess()) {
            if (HANDLE thread = CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr))
                CloseHandle(thread);
        }
    }
    return TRUE;
}
