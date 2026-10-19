#pragma once

#include "render/map_texture.h"

#include <d3d11.h>

namespace render {

struct MinimapFrame {
    float left, top, size;        // on-screen square in pixels
    float viewportW, viewportH;
    float playerX, playerZ;
    float headingRad;
    bool hasTruck;
    bool rotateWithTruck;
    float metersAcross;
    float opacity;
};

// A texture plus the world rectangle it covers. A null srv disables the layer.
struct MapLayer {
    ID3D11ShaderResourceView* srv = nullptr;
    MapRect rect;
};

// Inputs of the game's map-screen terrain shading (see NavTextures and PakAssets). Used instead of
// the colour layer when every texture is present.
struct NavInputs {
    ID3D11ShaderResourceView* tint = nullptr;
    ID3D11ShaderResourceView* cloaking = nullptr;
    ID3D11ShaderResourceView* height = nullptr;
    ID3D11ShaderResourceView* detail = nullptr;
    ID3D11ShaderResourceView* lut = nullptr;
    unsigned cloakingSize = 0;
    unsigned heightSize = 0;
    float maxHeight = 0;
    float exposure = 1.47f;

    bool Complete() const { return tint && cloaking && height && detail && lut && cloakingSize && heightSize; }
};

// Draws the round minimap with its own pipeline state. The caller is responsible for isolating
// the game's state (see overlay.cpp, which swaps in a private device context state).
class MinimapRenderer {
public:
    bool Init(ID3D11Device* device);
    // relief is only applied over an unshaded colour layer (the game's terrain bake).
    void Draw(ID3D11DeviceContext* context, const MinimapFrame& frame, const MapLayer& colour, const MapLayer& relief,
              const NavInputs& nav);
    void Release();

private:
    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_ps = nullptr;
    ID3D11Buffer* m_constants = nullptr;
    ID3D11BlendState* m_blend = nullptr;
    ID3D11SamplerState* m_sampler = nullptr;
    ID3D11SamplerState* m_wrap = nullptr;
    ID3D11SamplerState* m_linear = nullptr;
    ID3D11RasterizerState* m_raster = nullptr;
    ID3D11DepthStencilState* m_depth = nullptr;
};

} // namespace render
