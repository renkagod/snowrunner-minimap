#include "render/pak.h"

#include "core/paths.h"

#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace render {

Pak::~Pak()
{
    if (m_zip) {
        mz_zip_reader_end(static_cast<mz_zip_archive*>(m_zip));
        delete static_cast<mz_zip_archive*>(m_zip);
    }
    if (m_file)
        fclose(static_cast<FILE*>(m_file));
}

bool Pak::Open(const std::filesystem::path& path)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return false;
    auto* zip = new mz_zip_archive{};
    if (!mz_zip_reader_init_cfile(zip, file, 0, 0)) {
        delete zip;
        fclose(file);
        return false;
    }
    m_zip = zip;
    m_file = file;
    return true;
}

bool Pak::Read(const std::string& name, std::vector<unsigned char>& out)
{
    if (!m_zip)
        return false;
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(static_cast<mz_zip_archive*>(m_zip), name.c_str(), &size, 0);
    if (!data)
        return false;
    const auto* bytes = static_cast<unsigned char*>(data);
    out.assign(bytes, bytes + size);
    mz_free(data);
    return true;
}

bool Pak::ReadPct(const std::string& name, std::vector<unsigned char>& out)
{
    if (!m_zip)
        return false;
    auto* zip = static_cast<mz_zip_archive*>(m_zip);
    mz_zip_archive_file_stat header;
    const int headerIndex = mz_zip_reader_locate_file(zip, (name + "_header").c_str(), nullptr, 0);
    if (headerIndex < 0 || !mz_zip_reader_file_stat(zip, headerIndex, &header))
        return false;
    std::vector<unsigned char> whole;
    if (!Read(name, whole) || header.m_uncomp_size >= whole.size())
        return false;
    out.assign(whole.begin() + static_cast<ptrdiff_t>(header.m_uncomp_size), whole.end());
    return true;
}

ID3D11ShaderResourceView* Pak::LoadTexture(ID3D11Device* device, const std::string& name, bool srgb)
{
    // .pct_header ("TCIP" at 6): width at 0x10, height at 0x14, format code at 0x26, mip count at 0x30.
    std::vector<unsigned char> header, texels;
    if (!Read(name + "_header", header) || header.size() < 0x34 || std::memcmp(header.data() + 6, "TCIP", 4) != 0 ||
        !ReadPct(name, texels))
        return nullptr;
    uint32_t width, height, format, mips;
    std::memcpy(&width, header.data() + 0x10, 4);
    std::memcpy(&height, header.data() + 0x14, 4);
    std::memcpy(&format, header.data() + 0x26, 4);
    std::memcpy(&mips, header.data() + 0x30, 4);
    // 0x33 and 0x34 both hold BC7 blocks (checked by decoding); 0x0f is BC2 (the level colour maps,
    // which the game views as BC2). Other codes are not handled yet.
    DXGI_FORMAT dxgi;
    if (format == 0x33 || format == 0x34)
        dxgi = srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    else if (format == 0x0f)
        dxgi = srgb ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
    else
        return nullptr;
    if (!width || !height || width > 16384 || height > 16384 || !mips || mips > 15)
        return nullptr;

    std::vector<D3D11_SUBRESOURCE_DATA> levels(mips);
    size_t offset = 0;
    for (uint32_t i = 0; i < mips; ++i) {
        const uint32_t bw = std::max(1u, ((width >> i) + 3) / 4), bh = std::max(1u, ((height >> i) + 3) / 4);
        levels[i] = {texels.data() + offset, bw * 16, 0};
        offset += size_t(bw) * bh * 16;
        if (offset > texels.size())
            return nullptr;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = mips;
    td.ArraySize = 1;
    td.Format = dxgi;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&td, levels.data(), &texture))) {
        device->CreateShaderResourceView(texture, nullptr, &srv);
        texture->Release();
    }
    return srv;
}

std::string Pak::LastError() const
{
    if (!m_zip)
        return "not open";
    return mz_zip_get_error_string(mz_zip_get_last_error(static_cast<mz_zip_archive*>(m_zip)));
}

std::filesystem::path Pak::ClientDir()
{
    return paths::ModDir().parent_path() / L"../../preload/paks/client";
}

} // namespace render
