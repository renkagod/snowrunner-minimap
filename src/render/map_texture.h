#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace render {

// World area covered by a map image: X at the left edge, Z at the top edge, signed spans.
struct MapRect {
    float xLeft = -1008, zTop = 1008, xSpan = 2016, zSpan = -2016;
};

// Image layer for the current level, loaded from SnowMinimap\maps\<level><suffix>.png; the world
// rectangle comes from <level>.ini. Used for the colour fallback ("") and the relief ("_relief").
// Decoding runs on a worker thread; the GPU upload happens in Update() on the render thread.
class MapTexture {
public:
    explicit MapTexture(std::string suffix) : m_suffix(std::move(suffix)) {}

    void RequestLevel(const std::string& level);
    void Update(ID3D11Device* device, ID3D11DeviceContext* context);
    void Release();

    ID3D11ShaderResourceView* Srv() const { return m_srv; }
    const MapRect& Rect() const { return m_rect; }
    const std::string& Status() const { return m_status; }

private:
    struct Decoded {
        uint64_t generation = 0;
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
        MapRect rect;
        std::string error;
    };

    static void Decode(std::string level, std::string suffix, uint64_t generation, std::shared_ptr<std::mutex> mutex,
                       std::shared_ptr<std::unique_ptr<Decoded>> slot);

    std::string m_suffix;
    std::string m_level;
    uint64_t m_generation = 0;
    std::shared_ptr<std::mutex> m_pendingMutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::unique_ptr<Decoded>> m_pending = std::make_shared<std::unique_ptr<Decoded>>();

    ID3D11Texture2D* m_texture = nullptr;
    ID3D11ShaderResourceView* m_srv = nullptr;
    MapRect m_rect;
    std::string m_status = "no level";
};

} // namespace render
