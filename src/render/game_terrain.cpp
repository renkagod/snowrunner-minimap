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

enum class Role { Bake, Tint, Cloaking, Height };

struct Candidate {
    ID3D11Texture2D* texture; // one reference held by us
    D3D11_TEXTURE2D_DESC desc;
    Role role;
    int views = 0; // shader resource views we made on it; kept until the view is released
};

std::mutex g_mutex;
std::vector<Candidate> g_candidates;

bool Has(UINT flags, UINT needed)
{
    return (flags & needed) == needed;
}

bool Classify(const D3D11_TEXTURE2D_DESC& d, Role& role)
{
    if (d.Width != d.Height || d.ArraySize != 1 || d.SampleDesc.Count != 1 || !Has(d.BindFlags, D3D11_BIND_SHADER_RESOURCE))
        return false;
    const bool target = Has(d.BindFlags, D3D11_BIND_RENDER_TARGET);
    switch (d.Format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        role = Role::Bake;
        return target && d.Width >= 1024 && d.MipLevels == 1;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        role = Role::Tint;
        return d.Width >= 2048;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
        role = Role::Cloaking;
        return target && d.Width >= 64 && d.Width < 2048 && d.MipLevels == 1;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
        role = Role::Height;
        return !target && d.Width >= 1025 && (d.Width & 1) == 1;
    default:
        return false;
    }
}

const char* RoleName(Role r)
{
    switch (r) {
    case Role::Bake: return "terrain bake";
    case Role::Tint: return "tint map";
    case Role::Cloaking: return "fog of war";
    case Role::Height: return "height map";
    }
    return "?";
}

// Textures may be created on the game's loader threads.
HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                              const D3D11_SUBRESOURCE_DATA* initial, ID3D11Texture2D** texture)
{
    const HRESULT hr = g_origCreateTexture2D(device, desc, initial, texture);
    Role role;
    if (SUCCEEDED(hr) && desc && texture && *texture && Classify(*desc, role)) {
        (*texture)->AddRef();
        std::lock_guard lock(g_mutex);
        g_candidates.push_back({*texture, *desc, role});
        logger::Info("Captured {} {}x{} format {} mips {} ({} tracked)", RoleName(role), desc->Width, desc->Height,
                     static_cast<int>(desc->Format), desc->MipLevels, g_candidates.size());
    }
    return hr;
}

// Views keep only an internal reference, so the public count is the game's plus our one.
bool GameStillUses(const Candidate& c)
{
    const ULONG count = c.texture->AddRef();
    c.texture->Release();
    return count > 2u;
}

bool NewerExistsLocked(std::vector<Candidate>::iterator it)
{
    return std::any_of(it + 1, g_candidates.end(), [&](const Candidate& c) { return c.role == it->role; });
}

// Drops textures the game no longer uses. Bake targets are dropped once only our reference is left.
// The map inputs cannot be judged that way (the game keeps only its views of some of them), so an
// older one goes once the game has made a newer one, e.g. on the next level load, and we no longer
// have a view on it. Caller holds g_mutex.
void PruneLocked()
{
    for (auto it = g_candidates.begin(); it != g_candidates.end();) {
        const bool stale = it->role == Role::Bake ? !GameStillUses(*it) : NewerExistsLocked(it);
        if (it->views == 0 && stale) {
            it->texture->Release();
            it = g_candidates.erase(it);
        } else {
            ++it;
        }
    }
}

// Newest live texture of a role, AddRef'd. Caller holds g_mutex.
Candidate* NewestLocked(Role role)
{
    for (auto it = g_candidates.rbegin(); it != g_candidates.rend(); ++it) {
        if (it->role == role)
            return &*it;
    }
    return nullptr;
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
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    // The map screen samples the level colour map as sRGB (checked in a capture: BC2_UNORM_SRGB view).
    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return f;
    }
}

} // namespace

bool InstallTextureHook(ID3D11Device* device)
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
        PruneLocked();
        // The full-resolution bake is the largest one; the game also keeps a half-size copy.
        const Candidate* best = nullptr;
        for (const Candidate& c : g_candidates) {
            if (c.role == Role::Bake && (!best || c.desc.Width > best->desc.Width))
                best = &c;
        }
        if (best) {
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

namespace {

// Points view at the newest texture of a role, recreating the SRV when the game made a new one.
// Caller holds g_mutex.
void RefreshLocked(ID3D11Device* device, Role role, NavTextures::View& view)
{
    Candidate* c = NewestLocked(role);
    if (c && c->texture == view.texture)
        return;

    if (view.srv) {
        for (Candidate& old : g_candidates) {
            if (old.texture == view.texture)
                --old.views;
        }
        SafeRelease(view.srv);
        view.texture = nullptr;
        view.width = 0;
        logger::Info("Released {} view, the game made a newer one", RoleName(role));
    }
    if (!c)
        return;

    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = ViewFormat(c->desc.Format);
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = static_cast<UINT>(-1);
    if (FAILED(device->CreateShaderResourceView(c->texture, &desc, &view.srv))) {
        logger::Error("Cannot view {} ({}x{} format {})", RoleName(role), c->desc.Width, c->desc.Height,
                      static_cast<int>(c->desc.Format));
        view.srv = nullptr;
        return;
    }
    ++c->views;
    view.texture = c->texture;
    view.width = c->desc.Width;
    logger::Info("Using {} {}x{}", RoleName(role), c->desc.Width, c->desc.Height);
}

} // namespace

void NavTextures::Update(ID3D11Device* device)
{
    if (++m_frame % 30 != 1)
        return;
    std::lock_guard lock(g_mutex);
    RefreshLocked(device, Role::Tint, m_tint);
    RefreshLocked(device, Role::Cloaking, m_cloaking);
    RefreshLocked(device, Role::Height, m_height);
    PruneLocked();
}

void NavTextures::Release()
{
    std::lock_guard lock(g_mutex);
    for (View* v : {&m_tint, &m_cloaking, &m_height}) {
        for (Candidate& c : g_candidates) {
            if (c.texture == v->texture)
                --c.views;
        }
        SafeRelease(v->srv);
        v->texture = nullptr;
    }
}

std::string NavTextures::Status() const
{
    return std::format("map shader inputs: tint {}, fog {}, height {}", m_tint.width, m_cloaking.width, m_height.width);
}

} // namespace render
