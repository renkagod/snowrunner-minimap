#pragma once

#include <d3d11.h>

#include <string>

namespace render {

// Textures the map-screen shading needs that the game ships in preload/paks/client/boot.pak:
// the detail noise (env_minimap_detail, BC7 1024x1024) and the colour grading LUT
// (env_lut_minimap, 16x16x16 B8G8R8A8).
class PakAssets {
public:
    // Loads once; later calls are no-ops. Render thread only.
    void Load(ID3D11Device* device);
    void Release();

    bool Ready() const { return m_detail && m_lut; }
    ID3D11ShaderResourceView* Detail() const { return m_detail; }
    ID3D11ShaderResourceView* Lut() const { return m_lut; }
    const std::string& Status() const { return m_status; }

private:
    bool m_tried = false;
    ID3D11ShaderResourceView* m_detail = nullptr;
    ID3D11ShaderResourceView* m_lut = nullptr;
    std::string m_status = "not loaded";
};

} // namespace render
