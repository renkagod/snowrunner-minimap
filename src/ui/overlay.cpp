#include "ui/overlay.h"

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "game/game_state.h"
#include "game/memory.h"
#include "dev/renderdoc.h"
#include "render/game_terrain.h"
#include "render/map_texture.h"
#include "render/minimap_renderer.h"

#include <d3d11_1.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace overlay {

namespace {

bool g_initialized = false;
bool g_failed = false;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11DeviceContext1* g_context1 = nullptr;
ID3DDeviceContextState* g_ourState = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;
std::string g_imguiIni;

render::MinimapRenderer g_renderer;
render::GameTerrain g_gameTerrain;
render::MapTexture g_map("");          // colour image from the paks, used until the game's bake is found
render::MapTexture g_relief("_relief"); // shading laid over the game's unshaded bake

// The game's terrain bake is 1 px per metre from the terrain's north-west corner.
constexpr float kGameTerrainMetersPerPixel = 1.0f;

bool g_debugVisible = false;
bool g_keyDown[256] = {};
DXGI_FORMAT g_backBufferFormat = DXGI_FORMAT_UNKNOWN;
uint32_t g_backBufferW = 0, g_backBufferH = 0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_debugVisible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 1;
    return CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
}

// Typeless back buffers need a concrete view format. Non-sRGB keeps shader colours as written.
DXGI_FORMAT ViewFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

bool Init(IDXGISwapChain* swapChain)
{
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device)))) {
        logger::Error("Swap chain has no D3D11 device");
        return false;
    }
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    // A private context state lets us draw with any pipeline settings and hand the game back
    // its exact state afterwards, without tracking what we touched.
    ID3D11Device1* device1 = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&device1))) &&
        SUCCEEDED(g_context->QueryInterface(IID_PPV_ARGS(&g_context1)))) {
        const D3D_FEATURE_LEVEL level = g_device->GetFeatureLevel();
        const UINT flags = (g_device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                               ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                               : 0;
        device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                          &g_ourState);
        device1->Release();
    }
    if (!g_ourState) {
        logger::Error("Could not create a private device context state; overlay disabled");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    g_imguiIni = (paths::ModDir() / "imgui.ini").string();
    io.IniFilename = g_imguiIni.c_str();
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;

    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        logger::Error("ImGui backend init failed");
        return false;
    }
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));

    if (!g_renderer.Init(g_device))
        return false;
    render::GameTerrain::InstallHook(g_device);

    logger::Info("Overlay ready: hwnd={} feature level {:#x}, creation flags {:#x}", static_cast<void*>(g_hwnd),
                 static_cast<unsigned>(g_device->GetFeatureLevel()), g_device->GetCreationFlags());
    return true;
}

bool Pressed(int vk)
{
    if (vk <= 0 || vk > 255)
        return false;
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !g_keyDown[vk];
    g_keyDown[vk] = down;
    return pressed;
}

void PollKeys()
{
    if (GetForegroundWindow() != g_hwnd)
        return;
    config::Settings& cfg = config::Get();
    bool changed = false;
    if (Pressed(cfg.keyToggle)) {
        cfg.enabled = !cfg.enabled;
        changed = true;
    }
    if (Pressed(cfg.keyDebug)) {
        g_debugVisible = !g_debugVisible;
        ImGui::GetIO().MouseDrawCursor = g_debugVisible;
    }
    if (Pressed(cfg.keyZoomIn)) {
        cfg.metersAcross = std::max(25.0f, cfg.metersAcross / 1.25f);
        changed = true;
    }
    if (Pressed(cfg.keyZoomOut)) {
        cfg.metersAcross = std::min(4000.0f, cfg.metersAcross * 1.25f);
        changed = true;
    }
    if (changed)
        config::Save();
}

std::string ExeOffset(uintptr_t address)
{
    return address ? std::format("exe+{:#x}", address - mem::Game().base) : std::string("not found");
}

void DrawDebugWindow(const game::TruckPose& pose)
{
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("SnowMinimap", &g_debugVisible)) {
        ImGui::End();
        return;
    }

    const game::ScanStatus scan = game::GetStatus();
    if (ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Level: %s", scan.level.empty() ? "(none)" : scan.level.c_str());
        ImGui::Text("Session slot: %s (scans: %d)", ExeOffset(scan.levelSlot).c_str(), scan.levelScans);
        ImGui::Text("TRUCK_CONTROL: %s, vtables %zu (scans: %d)", ExeOffset(scan.truckSlot).c_str(), scan.truckVtables,
                    scan.truckScans);
        ImGui::Text("Back buffer: %ux%u format %d", g_backBufferW, g_backBufferH, static_cast<int>(g_backBufferFormat));
    }

    if (ImGui::CollapsingHeader("Truck", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (pose.chassisBody) {
            ImGui::Text("Chassis body: %p %s", reinterpret_cast<void*>(pose.chassisBody),
                        pose.valid ? "" : "(failed validation)");
            ImGui::Text("Position  X %9.2f  Y %8.2f  Z %9.2f", pose.position.x, pose.position.y, pose.position.z);
            for (int i = 0; i < 3; ++i)
                ImGui::Text("Axis +%#x  %6.3f %6.3f %6.3f", 0x90 + i * 0x10, pose.axis[i].x, pose.axis[i].y,
                            pose.axis[i].z);
            ImGui::Text("Heading %.1f deg", pose.headingRad * 180.0f / 3.14159265f);
        } else {
            ImGui::TextUnformatted("No truck (menu, garage or pointer chain not resolved)");
        }
    }

    if (ImGui::CollapsingHeader("Map", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted(g_gameTerrain.Status().c_str());
        ImGui::TextUnformatted(g_map.Status().c_str());
        ImGui::TextUnformatted(g_relief.Status().c_str());
        const render::MapRect& r = g_relief.Rect();
        ImGui::Text("Covers X %.0f..%.0f, Z %.0f..%.0f", r.xLeft, r.xLeft + r.xSpan, r.zTop + r.zSpan, r.zTop);
    }

    config::Settings& cfg = config::Get();
    if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show minimap", &cfg.enabled);
        ImGui::SameLine();
        ImGui::Checkbox("Rotate with truck", &cfg.rotateWithTruck);
        ImGui::SliderInt("Size (px at 1080p)", &cfg.sizePx, 120, 700);
        ImGui::SliderFloat("Metres across", &cfg.metersAcross, 25, 4000, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Opacity", &cfg.opacity, 0.1f, 1.0f);
        const char* anchors[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        int anchor = static_cast<int>(cfg.anchor);
        if (ImGui::Combo("Corner", &anchor, anchors, 4))
            cfg.anchor = static_cast<config::Anchor>(anchor);
        ImGui::SliderInt("Margin X", &cfg.marginX, 0, 600);
        ImGui::SliderInt("Margin Y", &cfg.marginY, 0, 600);
        ImGui::SeparatorText("Calibration");
        ImGui::SliderFloat("Heading offset", &cfg.headingOffsetDeg, -180, 180, "%.0f deg");
        ImGui::Checkbox("Mirror heading", &cfg.headingMirror);
        if (ImGui::Button("Save"))
            config::Save();
        ImGui::SameLine();
        if (ImGui::Button("Reload from file"))
            config::Load();
    }
    ImGui::TextDisabled("Keys: toggle %#x, debug %#x, zoom %#x / %#x", cfg.keyToggle, cfg.keyDebug, cfg.keyZoomIn,
                        cfg.keyZoomOut);
    ImGui::End();
}

} // namespace

void OnPresent(IDXGISwapChain* swapChain)
{
    if (g_failed)
        return;
    if (!g_initialized) {
        g_initialized = true;
        if (!Init(swapChain)) {
            g_failed = true;
            return;
        }
    }

    PollKeys();
    dev::OnFrame();

    const std::string level = game::CurrentLevel();
    const bool inLevel = !level.empty() && level != "level_main_menu";
    g_map.RequestLevel(level);
    g_relief.RequestLevel(level);
    const game::TruckPose pose = game::ReadTruck();

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return;
    D3D11_TEXTURE2D_DESC bbDesc;
    backBuffer->GetDesc(&bbDesc);
    if (bbDesc.Format != g_backBufferFormat || bbDesc.Width != g_backBufferW || bbDesc.Height != g_backBufferH) {
        logger::Info("Back buffer {}x{} format {} samples {}", bbDesc.Width, bbDesc.Height,
                     static_cast<int>(bbDesc.Format), bbDesc.SampleDesc.Count);
        g_backBufferFormat = bbDesc.Format;
        g_backBufferW = bbDesc.Width;
        g_backBufferH = bbDesc.Height;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = ViewFormat(bbDesc.Format);
    rtvDesc.ViewDimension = bbDesc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    const HRESULT hr = g_device->CreateRenderTargetView(backBuffer, &rtvDesc, &rtv);
    backBuffer->Release();
    if (FAILED(hr))
        return;

    ID3DDeviceContextState* gameState = nullptr;
    g_context1->SwapDeviceContextState(g_ourState, &gameState);

    g_map.Update(g_device, g_context);
    g_relief.Update(g_device, g_context);
    g_gameTerrain.Update(g_device, g_context);
    g_context->OMSetRenderTargets(1, &rtv, nullptr);

    const config::Settings& cfg = config::Get();
    if (cfg.enabled && inLevel) {
        const float scale = bbDesc.Height / 1080.0f;
        const float size = cfg.sizePx * scale;
        const float mx = cfg.marginX * scale, my = cfg.marginY * scale;
        const bool right = cfg.anchor == config::Anchor::TopRight || cfg.anchor == config::Anchor::BottomRight;
        const bool bottom = cfg.anchor == config::Anchor::BottomLeft || cfg.anchor == config::Anchor::BottomRight;

        render::MinimapFrame frame{};
        frame.left = right ? bbDesc.Width - mx - size : mx;
        frame.top = bottom ? bbDesc.Height - my - size : my;
        frame.size = size;
        frame.viewportW = static_cast<float>(bbDesc.Width);
        frame.viewportH = static_cast<float>(bbDesc.Height);
        frame.playerX = pose.position.x;
        frame.playerZ = pose.position.z;
        frame.headingRad = pose.headingRad;
        frame.hasTruck = pose.valid;
        frame.rotateWithTruck = cfg.rotateWithTruck;
        frame.metersAcross = cfg.metersAcross;
        frame.opacity = cfg.opacity;
        // Prefer the game's own terrain colour, shaded with our relief; fall back to the pak image.
        render::MapLayer colour{g_map.Srv(), g_map.Rect()};
        render::MapLayer relief;
        if (g_gameTerrain.Srv() && g_relief.Srv()) {
            const render::MapRect& terrain = g_relief.Rect();
            colour.srv = g_gameTerrain.Srv();
            colour.rect = {terrain.xLeft, terrain.zTop, g_gameTerrain.Width() * kGameTerrainMetersPerPixel,
                           -(g_gameTerrain.Height() * kGameTerrainMetersPerPixel)};
            relief = {g_relief.Srv(), terrain};
        }
        g_renderer.Draw(g_context, frame, colour, relief);
    }

    if (g_debugVisible) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(bbDesc.Width), static_cast<float>(bbDesc.Height));
        ImGui::NewFrame();
        DrawDebugWindow(pose);
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!g_debugVisible) // closed with the window's X button
            ImGui::GetIO().MouseDrawCursor = false;
    }

    g_context1->SwapDeviceContextState(gameState, nullptr);
    if (gameState)
        gameState->Release();
    rtv->Release();
}

} // namespace overlay
