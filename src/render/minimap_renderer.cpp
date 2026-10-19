#include "render/minimap_renderer.h"

#include "core/log.h"

#include "minimap_ps.h"
#include "minimap_vs.h"

namespace render {

namespace {

// Mirrors cbuffer Minimap in minimap.hlsl.
struct alignas(16) Constants {
    float rect[4];
    float viewport[2];
    float player[2];
    float heading;
    float mapRotation;
    float metersAcross;
    float opacity;
    float mapRect[4];
    float reliefRect[4];
    float hasMap;
    float hasTruck;
    float hasRelief;
    float pad;
    float navTerrain[4];
    float navTexel[4];
    float navTone[4];
};
static_assert(sizeof(Constants) % 16 == 0);

template <class T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

bool MinimapRenderer::Init(ID3D11Device* device)
{
    bool ok = SUCCEEDED(device->CreateVertexShader(g_minimapVS, sizeof(g_minimapVS), nullptr, &m_vs)) &&
              SUCCEEDED(device->CreatePixelShader(g_minimapPS, sizeof(g_minimapPS), nullptr, &m_ps));

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_constants));

    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(device->CreateBlendState(&blend, &m_blend));

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 4;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(device->CreateSamplerState(&sampler, &m_sampler));
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    ok = ok && SUCCEEDED(device->CreateSamplerState(&sampler, &m_wrap));
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ok = ok && SUCCEEDED(device->CreateSamplerState(&sampler, &m_linear));

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(device->CreateRasterizerState(&raster, &m_raster));

    D3D11_DEPTH_STENCIL_DESC depth{};
    ok = ok && SUCCEEDED(device->CreateDepthStencilState(&depth, &m_depth));

    if (!ok) {
        logger::Error("Minimap renderer init failed");
        Release();
    }
    return ok;
}

void MinimapRenderer::Draw(ID3D11DeviceContext* context, const MinimapFrame& frame, const MapLayer& colour,
                           const MapLayer& relief, const NavInputs& nav)
{
    const bool useNav = nav.Complete();
    if (!m_vs)
        return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(m_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    auto* c = static_cast<Constants*>(mapped.pData);
    *c = {};
    c->rect[0] = frame.left;
    c->rect[1] = frame.top;
    c->rect[2] = frame.size;
    c->rect[3] = frame.size;
    c->viewport[0] = frame.viewportW;
    c->viewport[1] = frame.viewportH;
    c->player[0] = frame.playerX;
    c->player[1] = frame.playerZ;
    c->heading = frame.headingRad;
    c->mapRotation = frame.rotateWithTruck && frame.hasTruck ? frame.headingRad : 0.0f;
    c->metersAcross = frame.metersAcross;
    c->opacity = frame.opacity;
    const auto setRect = [](float* out, const MapRect& r) {
        out[0] = r.xLeft;
        out[1] = r.zTop;
        out[2] = r.xSpan;
        out[3] = r.zSpan;
    };
    setRect(c->mapRect, colour.rect);
    setRect(c->reliefRect, relief.rect);
    c->hasMap = colour.srv ? 1.0f : 0.0f;
    c->hasTruck = frame.hasTruck ? 1.0f : 0.0f;
    c->hasRelief = relief.srv ? 1.0f : 0.0f;
    if (useNav) {
        // Terrain rectangle as the game's shaders get it (g_vTerrainSize): heights are 0.5 m apart
        // and centred on the origin, the colour and fog maps span half a metre more.
        const float n = static_cast<float>(nav.heightSize);
        c->navTerrain[0] = c->navTerrain[1] = -(n - 1) * 0.25f;
        c->navTerrain[2] = (n + 1) * 0.5f;
        c->navTerrain[3] = nav.maxHeight > 0 ? nav.maxHeight : 200.0f;
        c->navTexel[0] = 1.0f / nav.cloakingSize;
        c->navTexel[1] = 1.0f / n;
        c->navTexel[3] = 1.0f;
        c->navTone[0] = nav.exposure;
        c->navTone[1] = 6.273f; // captured white scale
        c->navTone[2] = 50.0f;  // g_vDetailTiling
        c->navTone[3] = 2.0f;   // g_fDetailPower
    }
    context->Unmap(m_constants, 0);

    D3D11_VIEWPORT vp{0, 0, frame.viewportW, frame.viewportH, 0, 1};
    context->RSSetViewports(1, &vp);
    context->RSSetState(m_raster);
    context->OMSetBlendState(m_blend, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depth, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vs, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &m_constants);
    context->PSSetShader(m_ps, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &m_constants);
    ID3D11ShaderResourceView* srvs[7] = {colour.srv, relief.srv};
    if (useNav) {
        srvs[2] = nav.tint;
        srvs[3] = nav.cloaking;
        srvs[4] = nav.height;
        srvs[5] = nav.detail;
        srvs[6] = nav.lut;
    }
    context->PSSetShaderResources(0, 7, srvs);
    ID3D11SamplerState* samplers[3] = {m_sampler, m_wrap, m_linear};
    context->PSSetSamplers(0, 3, samplers);
    context->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrvs[7] = {};
    context->PSSetShaderResources(0, 7, nullSrvs);
}

void MinimapRenderer::Release()
{
    SafeRelease(m_depth);
    SafeRelease(m_raster);
    SafeRelease(m_sampler);
    SafeRelease(m_wrap);
    SafeRelease(m_linear);
    SafeRelease(m_blend);
    SafeRelease(m_constants);
    SafeRelease(m_ps);
    SafeRelease(m_vs);
}

} // namespace render
