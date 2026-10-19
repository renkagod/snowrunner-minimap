#include "render/game_terrain.h"

#include "core/log.h"

#include <MinHook.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <vector>

namespace render {

namespace {

using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                       const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
CreateTexture2DFn g_origCreateTexture2D = nullptr;

struct Candidate {
    ID3D11Texture2D* texture; // one reference held by us
    D3D11_TEXTURE2D_DESC desc;
};

std::mutex g_mutex;
std::vector<Candidate> g_candidates;

bool LooksLikeTerrainTarget(const D3D11_TEXTURE2D_DESC& d)
{
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const UINT needed = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return bgra && d.Width == d.Height && d.Width >= 1024 && d.MipLevels == 1 && d.ArraySize == 1 &&
           d.SampleDesc.Count == 1 && (d.BindFlags & needed) == needed;
}

// Textures may be created on the game's loader threads.
HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                              const D3D11_SUBRESOURCE_DATA* initial, ID3D11Texture2D** texture)
{
    const HRESULT hr = g_origCreateTexture2D(device, desc, initial, texture);
    if (SUCCEEDED(hr) && desc && texture && *texture && LooksLikeTerrainTarget(*desc)) {
        (*texture)->AddRef();
        std::lock_guard lock(g_mutex);
        g_candidates.push_back({*texture, *desc});
        logger::Info("Terrain target candidate {}x{} format {} ({} tracked)", desc->Width, desc->Height,
                     static_cast<int>(desc->Format), g_candidates.size());
    }
    return hr;
}

// Our reference is the only one left once the game has released the texture.
bool OnlyWeHoldIt(ID3D11Texture2D* texture)
{
    const ULONG count = texture->AddRef();
    texture->Release();
    return count <= 2;
}

template <class T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : f;
}

} // namespace

bool GameTerrain::InstallHook(ID3D11Device* device)
{
    void* target = (*reinterpret_cast<void***>(device))[5]; // ID3D11Device::CreateTexture2D
    const bool ok = MH_CreateHook(target, &HookCreateTexture2D, reinterpret_cast<void**>(&g_origCreateTexture2D)) ==
                        MH_OK &&
                    MH_EnableHook(target) == MH_OK;
    logger::Info("CreateTexture2D hook {}", ok ? "installed" : "FAILED");
    return ok;
}

void GameTerrain::Update(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (++m_frame % 60 != 1)
        return;

    ID3D11Texture2D* source = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    {
        std::lock_guard lock(g_mutex);
        // Drop targets the game has freed (level unloaded).
        for (auto it = g_candidates.begin(); it != g_candidates.end();) {
            if (OnlyWeHoldIt(it->texture)) {
                it->texture->Release();
                it = g_candidates.erase(it);
            } else {
                ++it;
            }
        }
        // The full-resolution bake is the largest one; the game also keeps a half-size copy.
        const auto best = std::max_element(g_candidates.begin(), g_candidates.end(),
                                           [](const Candidate& a, const Candidate& b) { return a.desc.Width < b.desc.Width; });
        if (best != g_candidates.end()) {
            source = best->texture;
            source->AddRef();
            desc = best->desc;
        }
    }
    if (!source) {
        if (m_srv)
            logger::Info("Terrain target released by the game");
        SafeRelease(m_srv);
        SafeRelease(m_copy);
        m_width = m_height = 0;
        m_status = "waiting for the game's terrain target";
        return;
    }

    if (!m_copy || desc.Width != m_width || desc.Height != m_height) {
        SafeRelease(m_srv);
        SafeRelease(m_copy);
        D3D11_TEXTURE2D_DESC copy = desc;
        copy.MipLevels = 0;
        copy.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        copy.CPUAccessFlags = 0;
        copy.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        copy.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = ViewFormat(desc.Format);
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = static_cast<UINT>(-1);
        if (FAILED(device->CreateTexture2D(&copy, nullptr, &m_copy)) ||
            FAILED(device->CreateShaderResourceView(m_copy, &view, &m_srv))) {
            SafeRelease(m_copy);
            source->Release();
            m_status = "copy texture creation failed";
            logger::Error("Terrain copy creation failed ({}x{})", desc.Width, desc.Height);
            return;
        }
        m_width = desc.Width;
        m_height = desc.Height;
        logger::Info("Using terrain target {}x{}", m_width, m_height);
    }

    context->CopySubresourceRegion(m_copy, 0, 0, 0, 0, source, 0, nullptr);
    context->GenerateMips(m_srv);
    source->Release();
    m_status = std::format("game terrain {}x{}", m_width, m_height);
}

void GameTerrain::Release()
{
    SafeRelease(m_srv);
    SafeRelease(m_copy);
}

} // namespace render
