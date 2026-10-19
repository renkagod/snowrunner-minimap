#include "hooks/present_hook.h"

#include "core/log.h"
#include "ui/overlay.h"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

namespace hooks {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

static PresentFn g_origPresent = nullptr;
static Present1Fn g_origPresent1 = nullptr;

// Present1 may be implemented on top of Present on some runtimes; draw only once per frame.
static thread_local bool g_inPresent = false;

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    if (!g_inPresent && !(flags & DXGI_PRESENT_TEST)) {
        g_inPresent = true;
        overlay::OnPresent(swapChain);
        const HRESULT hr = g_origPresent(swapChain, syncInterval, flags);
        g_inPresent = false;
        return hr;
    }
    return g_origPresent(swapChain, syncInterval, flags);
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
{
    if (!g_inPresent && !(flags & DXGI_PRESENT_TEST)) {
        g_inPresent = true;
        overlay::OnPresent(swapChain);
        const HRESULT hr = g_origPresent1(swapChain, syncInterval, flags, params);
        g_inPresent = false;
        return hr;
    }
    return g_origPresent1(swapChain, syncInterval, flags, params);
}

static void* VtableEntry(void* object, int index)
{
    return (*static_cast<void***>(object))[index];
}

bool InstallPresentHook()
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SnowMinimapDummy";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                  wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           &desc, &swapChain, &device, nullptr, &context);
    if (FAILED(hr)) {
        logger::Error("Dummy swap chain creation failed: {:#x}", static_cast<unsigned>(hr));
        DestroyWindow(window);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    void* present = VtableEntry(swapChain, 8);
    void* present1 = nullptr;
    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1)))) {
        present1 = VtableEntry(swapChain1, 22);
        swapChain1->Release();
    }

    swapChain->Release();
    context->Release();
    device->Release();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    bool ok = MH_CreateHook(present, &HookPresent, reinterpret_cast<void**>(&g_origPresent)) == MH_OK;
    if (ok && present1)
        ok = MH_CreateHook(present1, &HookPresent1, reinterpret_cast<void**>(&g_origPresent1)) == MH_OK;
    ok = ok && MH_EnableHook(MH_ALL_HOOKS) == MH_OK;

    logger::Info("Present hooks {} (Present={}, Present1={})", ok ? "installed" : "FAILED", present, present1);
    return ok;
}

} // namespace hooks
