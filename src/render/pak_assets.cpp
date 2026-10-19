#include "render/pak_assets.h"

#include "core/log.h"
#include "render/pak.h"

#include <vector>

namespace render {

namespace {

template <class T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

void PakAssets::Load(ID3D11Device* device)
{
    if (m_tried)
        return;
    m_tried = true;

    Pak boot;
    if (!boot.Open(Pak::ClientDir() / L"boot.pak")) {
        m_status = "boot.pak not found";
        logger::Error("boot.pak: cannot open");
        return;
    }
    m_detail = boot.LoadTexture(device, "[textures]\\pct\\env_minimap_detail__s_d.pct", false);

    // The LUT is a 16x16x16 B8G8R8A8 volume, indexed [b][g][r].
    constexpr unsigned kLutSize = 16;
    std::vector<unsigned char> lut;
    if (boot.ReadPct("[textures]\\pct\\env_lut_minimap__vol_uncmp.pct", lut) &&
        lut.size() >= size_t(kLutSize) * kLutSize * kLutSize * 4) {
        D3D11_TEXTURE3D_DESC ld{};
        ld.Width = ld.Height = ld.Depth = kLutSize;
        ld.MipLevels = 1;
        ld.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        ld.Usage = D3D11_USAGE_IMMUTABLE;
        ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA lutData{lut.data(), kLutSize * 4, kLutSize * kLutSize * 4};
        ID3D11Texture3D* lutTex = nullptr;
        if (SUCCEEDED(device->CreateTexture3D(&ld, &lutData, &lutTex))) {
            device->CreateShaderResourceView(lutTex, nullptr, &m_lut);
            lutTex->Release();
        }
    }

    m_status = Ready() ? "map detail and LUT loaded" : "map detail or LUT unreadable";
    if (Ready())
        logger::Info("boot.pak: {}", m_status);
    else
        logger::Error("boot.pak: {} ({})", m_status, boot.LastError());
}

void PakAssets::Release()
{
    SafeRelease(m_detail);
    SafeRelease(m_lut);
}

} // namespace render
