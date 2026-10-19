#include "render/pak_assets.h"

#include "core/log.h"
#include "core/paths.h"

#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <vector>

namespace render {

namespace {

// A .pct file is a copy of its .pct_header entry followed by the raw texels (and a few spare bytes).
// Returns the texels.
bool ReadPct(mz_zip_archive& zip, const std::string& name, std::vector<unsigned char>& out)
{
    mz_zip_archive_file_stat header;
    const int headerIndex = mz_zip_reader_locate_file(&zip, (name + "_header").c_str(), nullptr, 0);
    if (headerIndex < 0 || !mz_zip_reader_file_stat(&zip, headerIndex, &header))
        return false;
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, name.c_str(), &size, 0);
    if (!data)
        return false;
    const auto* bytes = static_cast<unsigned char*>(data);
    if (header.m_uncomp_size < size)
        out.assign(bytes + header.m_uncomp_size, bytes + size);
    mz_free(data);
    return !out.empty();
}

size_t Bc7MipChainSize(unsigned size, unsigned mips)
{
    size_t total = 0;
    for (unsigned i = 0; i < mips; ++i) {
        const unsigned blocks = std::max(1u, ((size >> i) + 3) / 4);
        total += size_t(blocks) * blocks * 16;
    }
    return total;
}

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

    const auto pak = paths::ModDir().parent_path() / L"../../preload/paks/client/boot.pak";
    FILE* file = nullptr;
    if (_wfopen_s(&file, pak.c_str(), L"rb") != 0 || !file) {
        m_status = "boot.pak not found";
        logger::Error("Cannot open {}", pak.string());
        return;
    }
    mz_zip_archive zip{};
    std::vector<unsigned char> detail, lut;
    // The paks store entry names with backslashes.
    const bool opened = mz_zip_reader_init_cfile(&zip, file, 0, 0);
    const bool read = opened && ReadPct(zip, "[textures]\\pct\\env_minimap_detail__s_d.pct", detail) &&
                      ReadPct(zip, "[textures]\\pct\\env_lut_minimap__vol_uncmp.pct", lut);
    const char* error = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
    if (opened)
        mz_zip_reader_end(&zip);
    fclose(file);
    if (!read) {
        m_status = "boot.pak unreadable";
        logger::Error("boot.pak: {} ({})", opened ? "map detail or LUT entry unreadable" : "not a readable zip", error);
        return;
    }

    constexpr unsigned kDetailSize = 1024, kDetailMips = 11, kLutSize = 16;
    const size_t detailBytes = Bc7MipChainSize(kDetailSize, kDetailMips);
    const size_t lutBytes = size_t(kLutSize) * kLutSize * kLutSize * 4;
    if (detail.size() < detailBytes || lut.size() < lutBytes) {
        m_status = "unexpected map detail or LUT layout";
        logger::Error("boot.pak: detail {} bytes, LUT {} bytes", detail.size(), lut.size());
        return;
    }

    D3D11_SUBRESOURCE_DATA mips[kDetailMips];
    const unsigned char* texels = detail.data();
    for (unsigned i = 0; i < kDetailMips; ++i) {
        const unsigned blocks = std::max(1u, ((kDetailSize >> i) + 3) / 4);
        mips[i] = {texels, blocks * 16, 0};
        texels += size_t(blocks) * blocks * 16;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kDetailSize;
    td.MipLevels = kDetailMips;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_BC7_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* detailTex = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&td, mips, &detailTex))) {
        device->CreateShaderResourceView(detailTex, nullptr, &m_detail);
        detailTex->Release();
    }

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

    m_status = Ready() ? "map detail and LUT loaded" : "map detail or LUT texture creation failed";
    logger::Info("boot.pak: {}", m_status);
}

void PakAssets::Release()
{
    SafeRelease(m_detail);
    SafeRelease(m_lut);
}

} // namespace render
