#pragma once

#include <d3d11.h>

#include <filesystem>
#include <string>
#include <vector>

namespace render {

// Read access to one of the game's .pak files (plain zip archives). Entry names use backslashes,
// e.g. "[textures]\\pct\\env_lut_minimap__vol_uncmp.pct".
class Pak {
public:
    Pak() = default;
    Pak(const Pak&) = delete;
    Pak& operator=(const Pak&) = delete;
    ~Pak();

    bool Open(const std::filesystem::path& path);
    bool IsOpen() const { return m_zip != nullptr; }

    // Whole entry, decompressed.
    bool Read(const std::string& name, std::vector<unsigned char>& out);
    // Texels of a .pct texture: the entry minus the copy of its .pct_header entry it starts with
    // (a few spare bytes may trail the texels).
    bool ReadPct(const std::string& name, std::vector<unsigned char>& out);
    // A 2D .pct texture with its mip chain as an immutable texture's view; null if it is missing or
    // in a format not handled yet (BC7 and BC2 so far). srgb picks the sRGB view format.
    ID3D11ShaderResourceView* LoadTexture(ID3D11Device* device, const std::string& name, bool srgb);

    // miniz's description of the last failure.
    std::string LastError() const;

    // preload/paks/client next to the game's Sources/Bin folder.
    static std::filesystem::path ClientDir();

private:
    void* m_zip = nullptr; // mz_zip_archive
    void* m_file = nullptr; // FILE*
};

} // namespace render
