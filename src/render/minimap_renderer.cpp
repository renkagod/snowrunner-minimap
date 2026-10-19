#include "render/minimap_renderer.h"

#include "core/log.h"
#include "render/nav_models.h"

#include "minimap_model_ps.h"
#include "minimap_model_vs.h"
#include "minimap_object_vs.h"
#include "minimap_ps.h"
#include "minimap_scene_vs.h"
#include "minimap_terrain_ps.h"
#include "minimap_vs.h"

#include <algorithm>
#include <cmath>

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

// Mirrors cbuffer Model.
struct alignas(16) ModelConstants {
    float colour[4];
};

constexpr DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr unsigned kSceneSamples = 4;
// Largest landmark (a building or a bridge) reaching into the minimap from outside its circle.
constexpr float kModelReach = 60.0f;

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
              SUCCEEDED(device->CreatePixelShader(g_minimapPS, sizeof(g_minimapPS), nullptr, &m_ps)) &&
              SUCCEEDED(device->CreateVertexShader(g_sceneVS, sizeof(g_sceneVS), nullptr, &m_sceneVs)) &&
              SUCCEEDED(device->CreatePixelShader(g_terrainPS, sizeof(g_terrainPS), nullptr, &m_terrainPs)) &&
              SUCCEEDED(device->CreateVertexShader(g_modelVS, sizeof(g_modelVS), nullptr, &m_modelVs)) &&
              SUCCEEDED(device->CreatePixelShader(g_modelPS, sizeof(g_modelPS), nullptr, &m_modelPs)) &&
              SUCCEEDED(device->CreateVertexShader(g_objectVS, sizeof(g_objectVS), nullptr, &m_objectVs));

    // Mesh vertices (slot 0) and the engine's packed instances (slot 1), both 28 bytes.
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INSTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE", 2, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 20, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    ok = ok && SUCCEEDED(device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)), g_modelVS,
                                                   sizeof(g_modelVS), &m_modelLayout));
    // Single objects: the same vertices, and the rows of each object's world matrix (slot 1, 48 bytes).
    const D3D11_INPUT_ELEMENT_DESC objectLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INSTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    ok = ok && SUCCEEDED(device->CreateInputLayout(objectLayout, static_cast<UINT>(std::size(objectLayout)),
                                                   g_objectVS, sizeof(g_objectVS), &m_objectLayout));

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_constants));
    cb.ByteWidth = sizeof(ModelConstants);
    ok = ok && SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_modelConstants));

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
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ok = ok && SUCCEEDED(device->CreateDepthStencilState(&depth, &m_depthWrite));

    if (!ok) {
        logger::Error("Minimap renderer init failed");
        Release();
    }
    return ok;
}

bool MinimapRenderer::EnsureScene(ID3D11Device* device, unsigned size)
{
    if (m_sceneSrv && size == m_sceneSize)
        return true;
    ReleaseScene();
    m_sceneSize = size;

    UINT quality = 0;
    m_sceneSamples = SUCCEEDED(device->CheckMultisampleQualityLevels(kSceneFormat, kSceneSamples, &quality)) && quality
                         ? kSceneSamples
                         : 1;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = size;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = kSceneFormat;
    td.SampleDesc.Count = m_sceneSamples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    bool ok = SUCCEEDED(device->CreateTexture2D(&td, nullptr, &m_sceneMs)) &&
              SUCCEEDED(device->CreateRenderTargetView(m_sceneMs, nullptr, &m_sceneRtv));
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ok = ok && SUCCEEDED(device->CreateTexture2D(&td, nullptr, &m_sceneDepth)) &&
         SUCCEEDED(device->CreateDepthStencilView(m_sceneDepth, nullptr, &m_sceneDsv));
    td.Format = kSceneFormat;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ok = ok && SUCCEEDED(device->CreateTexture2D(&td, nullptr, &m_sceneResolved)) &&
         SUCCEEDED(device->CreateShaderResourceView(m_sceneResolved, nullptr, &m_sceneSrv));
    if (!ok) {
        logger::Error("Minimap offscreen target {}x{} ({} samples) could not be created", size, size, m_sceneSamples);
        ReleaseScene();
    }
    return ok;
}

void MinimapRenderer::ReleaseScene()
{
    SafeRelease(m_sceneSrv);
    SafeRelease(m_sceneResolved);
    SafeRelease(m_sceneDsv);
    SafeRelease(m_sceneDepth);
    SafeRelease(m_sceneRtv);
    SafeRelease(m_sceneMs);
    m_sceneSize = 0;
}

void MinimapRenderer::DrawScene(ID3D11DeviceContext* context, const MinimapFrame& frame, const NavInputs& nav)
{
    const float clear[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(m_sceneRtv, clear);
    context->ClearDepthStencilView(m_sceneDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    context->OMSetRenderTargets(1, &m_sceneRtv, m_sceneDsv);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(m_sceneSize), static_cast<float>(m_sceneSize), 0, 1};
    context->RSSetViewports(1, &vp);
    context->RSSetState(m_raster);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ID3D11Buffer* constants[2] = {m_constants, m_modelConstants};
    context->VSSetConstantBuffers(0, 2, constants);
    context->PSSetConstantBuffers(0, 2, constants);
    ID3D11SamplerState* samplers[3] = {m_sampler, m_wrap, m_linear};
    context->PSSetSamplers(0, 3, samplers);
    ID3D11ShaderResourceView* srvs[9] = {nullptr, nullptr, nav.tint, nav.cloaking, nav.height,
                                         nav.detail, nav.lut, nullptr, nullptr};
    context->PSSetShaderResources(0, 9, srvs);

    // Terrain.
    context->OMSetDepthStencilState(m_depth, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_sceneVs, nullptr, 0);
    context->PSSetShader(m_terrainPs, nullptr, 0);
    context->Draw(3, 0);

    // Landmark models.
    if (nav.models) {
        const float radius = frame.metersAcross * 0.5f + kModelReach;
        const auto& draws = nav.models->Collect(context, frame.playerX, frame.playerZ, radius);
        if (!draws.empty()) {
            context->OMSetDepthStencilState(m_depthWrite, 0);
            context->PSSetShader(m_modelPs, nullptr, 0);
            bool objects = !draws.front().object;
            for (const NavModelDraw& draw : draws) {
                if (draw.object != objects) {
                    objects = draw.object;
                    context->IASetInputLayout(objects ? m_objectLayout : m_modelLayout);
                    context->VSSetShader(objects ? m_objectVs : m_modelVs, nullptr, 0);
                }
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (FAILED(context->Map(m_modelConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                    break;
                auto* mc = static_cast<ModelConstants*>(mapped.pData);
                for (int i = 0; i < 4; ++i)
                    mc->colour[i] = draw.colour[i];
                context->Unmap(m_modelConstants, 0);
                ID3D11Buffer* vbs[2] = {draw.vb,
                                        draw.object ? nav.models->ObjectBuffer() : nav.models->InstanceBuffer()};
                const UINT strides[2] = {28, draw.object ? UINT(sizeof(NavObjectInstance)) : 28u};
                const UINT offsets[2] = {0, 0};
                context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
                context->IASetIndexBuffer(draw.ib, draw.indexFormat, 0);
                context->PSSetShaderResources(8, 1, &draw.albedo);
                context->DrawIndexedInstanced(draw.indexCount, draw.instanceCount, 0, 0, draw.startInstance);
            }
        }
    }

    ID3D11ShaderResourceView* nullSrvs[9] = {};
    context->PSSetShaderResources(0, 9, nullSrvs);
    if (m_sceneSamples > 1)
        context->ResolveSubresource(m_sceneResolved, 0, m_sceneMs, 0, kSceneFormat);
    else
        context->CopyResource(m_sceneResolved, m_sceneMs);
}

void MinimapRenderer::Draw(ID3D11DeviceContext* context, const MinimapFrame& frame, const MapLayer& colour,
                           const MapLayer& relief, const NavInputs& nav)
{
    if (!m_vs)
        return;
    bool useNav = nav.Complete();
    if (useNav) {
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        useNav = EnsureScene(device, static_cast<unsigned>(std::lround(std::max(frame.size, 16.0f))));
        device->Release();
    }

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

    // The caller's target (the back buffer) is restored after the offscreen passes.
    ID3D11RenderTargetView* target = nullptr;
    context->OMGetRenderTargets(1, &target, nullptr);
    if (useNav)
        DrawScene(context, frame, nav);
    context->OMSetRenderTargets(1, &target, nullptr);
    if (target)
        target->Release();

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
    ID3D11ShaderResourceView* srvs[8] = {colour.srv, relief.srv};
    if (useNav) {
        srvs[6] = nav.lut;
        srvs[7] = m_sceneSrv;
    }
    context->PSSetShaderResources(0, 8, srvs);
    ID3D11SamplerState* samplers[3] = {m_sampler, m_wrap, m_linear};
    context->PSSetSamplers(0, 3, samplers);
    context->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrvs[8] = {};
    context->PSSetShaderResources(0, 8, nullSrvs);
}

void MinimapRenderer::Release()
{
    ReleaseScene();
    SafeRelease(m_depthWrite);
    SafeRelease(m_depth);
    SafeRelease(m_raster);
    SafeRelease(m_sampler);
    SafeRelease(m_wrap);
    SafeRelease(m_linear);
    SafeRelease(m_blend);
    SafeRelease(m_modelConstants);
    SafeRelease(m_constants);
    SafeRelease(m_modelLayout);
    SafeRelease(m_objectLayout);
    SafeRelease(m_objectVs);
    SafeRelease(m_modelPs);
    SafeRelease(m_modelVs);
    SafeRelease(m_terrainPs);
    SafeRelease(m_sceneVs);
    SafeRelease(m_ps);
    SafeRelease(m_vs);
}

} // namespace render
