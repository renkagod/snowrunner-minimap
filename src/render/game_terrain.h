#pragma once

#include <d3d11.h>

#include <string>

namespace render {

// The engine bakes a top-down colour image of the whole level into a square B8G8R8A8 render
// target (2604x2604 on a 2x2 km map, 1 px per metre from the terrain's north-west corner), a few
// tiles per frame. This class spots that target as the game creates it and keeps a mipmapped copy.
class GameTerrain {
public:
    // Hooks ID3D11Device::CreateTexture2D. Call once with the game's device.
    static bool InstallHook(ID3D11Device* device);

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

} // namespace render
