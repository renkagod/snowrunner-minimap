#include "dev/renderdoc.h"

#include "core/log.h"
#include "core/paths.h"

#include <renderdoc_app.h>
#include <windows.h>

namespace dev {

static RENDERDOC_API_1_6_0* g_api = nullptr;
static std::filesystem::path g_trigger;
static unsigned g_frame = 0;

void LoadRenderDoc(const std::wstring& dllPath)
{
    if (dllPath.empty())
        return;

    HMODULE module = LoadLibraryW(dllPath.c_str());
    if (!module) {
        logger::Error("RenderDoc not loaded from configured path (error {})", GetLastError());
        return;
    }
    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_6_0* api = nullptr;
    if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api)) || !api) {
        logger::Error("RenderDoc loaded but its in-app API is unavailable");
        return;
    }

    const std::filesystem::path dir = paths::ModDir() / "captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    api->SetCaptureFilePathTemplate((dir / "snowrunner").string().c_str());
    // Callstacks tie each API call back to the game code that issued it.
    const int stacks = api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 1);
    RENDERDOC_InputButton key = eRENDERDOC_Key_F12;
    api->SetCaptureKeys(&key, 1);

    int major = 0, minor = 0, patch = 0;
    api->GetAPIVersion(&major, &minor, &patch);
    logger::Info("RenderDoc {}.{}.{} loaded, captures in {}, callstacks option {}", major, minor, patch, dir.string(),
                 stacks ? "accepted" : "rejected");
    g_api = api;
    g_trigger = dir / "trigger";
}

void OnFrame()
{
    if (!g_api || ++g_frame % 30 != 0)
        return;
    std::error_code ec;
    if (!std::filesystem::exists(g_trigger, ec))
        return;
    std::filesystem::remove(g_trigger, ec);
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 1);
    g_api->TriggerCapture();
    logger::Info("RenderDoc capture triggered (callstacks {})",
                 g_api->GetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks) ? "on" : "off");
}

} // namespace dev
