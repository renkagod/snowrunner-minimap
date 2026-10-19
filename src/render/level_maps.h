#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

namespace render {

// Map-screen inputs that do not need the map screen to have been opened: the level colour map
// (shared_textures.pak, [textures]\pct\<level>_map.pct, the file the map screen loads as map.dds)
// and the live fog of war, copied from the engine's memory a few times a second. Render thread only.
class LevelMaps {
public:
    void Update(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& level);
    void Release();

    ID3D11ShaderResourceView* Tint() const { return m_tint; }
    ID3D11ShaderResourceView* Fog() const { return m_fog; }
    unsigned FogSize() const { return m_fogWidth; }
    const std::string& Status() const { return m_status; }

private:
    void UpdateFog(ID3D11Device* device, ID3D11DeviceContext* context);

    std::string m_level;
    ID3D11ShaderResourceView* m_tint = nullptr;
    ID3D11Texture2D* m_fogTexture = nullptr;
    ID3D11ShaderResourceView* m_fog = nullptr;
    unsigned m_fogWidth = 0, m_fogHeight = 0;
    std::vector<uint8_t> m_cells;
    int m_frame = 0;
    std::string m_tintStatus = "no level";
    std::string m_status = "no level";
};

} // namespace render
