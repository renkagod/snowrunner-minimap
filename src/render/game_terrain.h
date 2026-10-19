#pragma once

#include <d3d11.h>

#include <string>

namespace render {

// Hooks ID3D11Device::CreateTexture2D and keeps a reference to the level textures the minimap
// reuses, as the game creates them during level load. Call once with the game's device.
bool InstallTextureHook(ID3D11Device* device);

// The engine bakes a top-down colour image of the whole level into a square B8G8R8A8 render
// target (2604x2604 on a 2x2 km map, 1 px per metre from the terrain's north-west corner), a few
// tiles per frame. This class keeps a mipmapped copy of it.
class GameTerrain {
public:
    // Refreshes the copy about once a second. Render thread only.
    void Update(ID3D11Device* device, ID3D11DeviceContext* context);
    void Release();

    ID3D11ShaderResourceView* Srv() const { return m_srv; }
    unsigned Width() const { return m_width; }
    unsigned Height() const { return m_height; }
    const std::string& Status() const { return m_status; }

private:
    ID3D11Texture2D* m_copy = nullptr;
    ID3D11ShaderResourceView* m_srv = nullptr;
    unsigned m_width = 0, m_height = 0;
    unsigned m_frame = 0;
    std::string m_status = "waiting for the game's terrain target";
};

// The inputs of the game's own map-screen terrain shader (MinimapNavigation.fxs), read live:
//  - tint: the level colour map, BC2, alpha marks land (4032x4032 on a 2 km map)
//  - cloaking: fog of war, R8 render target (1 = revealed, 0.5 = explored, 0 = hidden), updated by the game
//  - height: R16 height map, 0.5 m per texel
// Tint and cloaking cover the terrain rectangle with texture row 0 at its south edge.
class NavTextures {
public:
    void Update(ID3D11Device* device);
    void Release();

    bool Ready() const { return m_tint.srv && m_cloaking.srv && m_height.srv; }
    ID3D11ShaderResourceView* Tint() const { return m_tint.srv; }
    ID3D11ShaderResourceView* Cloaking() const { return m_cloaking.srv; }
    ID3D11ShaderResourceView* Height() const { return m_height.srv; }
    unsigned CloakingSize() const { return m_cloaking.width; }
    unsigned HeightSize() const { return m_height.width; }
    std::string Status() const;

    struct View {
        ID3D11Texture2D* texture = nullptr; // the game's texture this view was made for (not owned)
        ID3D11ShaderResourceView* srv = nullptr;
        unsigned width = 0;
    };

private:
    View m_tint, m_cloaking, m_height;
    unsigned m_frame = 0;
};

} // namespace render
