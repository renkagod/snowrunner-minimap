#include "render/level_maps.h"

#include "core/log.h"
#include "game/game_state.h"
#include "render/pak.h"

#include <format>

namespace render {

namespace {

constexpr int kFogInterval = 15; // frames between fog of war copies

template <class T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

void LevelMaps::Update(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& level)
{
    if (level != m_level) {
        m_level = level;
        Release();
        if (!level.empty() && level != "level_main_menu") {
            Pak pak;
            const std::string name = "[textures]\\pct\\" + level + "_map.pct";
            if (pak.Open(Pak::ClientDir() / L"shared_textures.pak"))
                m_tint = pak.LoadTexture(device, name, true); // the map screen views it as sRGB
            m_tintStatus = m_tint ? "colour map from shared_textures.pak" : "no colour map in shared_textures.pak";
            if (m_tint)
                logger::Info("Loaded {}", name);
            else
                logger::Warn("Cannot load {} from shared_textures.pak", name);
        }
    }
    if (!m_level.empty() && ++m_frame >= kFogInterval) {
        m_frame = 0;
        UpdateFog(device, context);
    }
    m_status = std::format("{}; live fog {}", m_tintStatus,
                           m_fog ? std::format("{}x{}", m_fogWidth, m_fogHeight) : std::string("not found"));
}

void LevelMaps::UpdateFog(ID3D11Device* device, ID3D11DeviceContext* context)
{
    unsigned width = 0, height = 0;
    if (!game::ReadFogOfWar(m_cells, width, height))
        return;
    if (width != m_fogWidth || height != m_fogHeight) {
        SafeRelease(m_fog);
        SafeRelease(m_fogTexture);
        m_fogWidth = m_fogHeight = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &m_fogTexture)) ||
            FAILED(device->CreateShaderResourceView(m_fogTexture, nullptr, &m_fog))) {
            SafeRelease(m_fogTexture);
            return;
        }
        m_fogWidth = width;
        m_fogHeight = height;
    }
    context->UpdateSubresource(m_fogTexture, 0, nullptr, m_cells.data(), width, 0);
}

void LevelMaps::Release()
{
    SafeRelease(m_tint);
    SafeRelease(m_fog);
    SafeRelease(m_fogTexture);
    m_fogWidth = m_fogHeight = 0;
    m_tintStatus = "no level";
}

} // namespace render
