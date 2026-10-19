#include "render/map_texture.h"

#include "core/log.h"
#include "core/paths.h"

#include <wincodec.h>
#include <windows.h>

#include <format>
#include <thread>

namespace render {

template <class T>
static void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static float ReadIniFloat(const std::wstring& ini, const wchar_t* key, float fallback)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(L"map", key, L"", buf, 64, ini.c_str());
    if (!buf[0])
        return fallback;
    return std::wcstof(buf, nullptr);
}

void MapTexture::Decode(std::string level, std::string suffix, uint64_t generation, std::shared_ptr<std::mutex> mutex,
                        std::shared_ptr<std::unique_ptr<Decoded>> slot)
{
    auto result = std::make_unique<Decoded>();
    result->generation = generation;

    const std::filesystem::path png = paths::MapsDir() / (level + suffix + ".png");
    const std::wstring ini = (paths::MapsDir() / (level + ".ini")).wstring();

    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = factory->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                &decoder);
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr))
        hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr))
        hr = converter->GetSize(&result->width, &result->height);
    if (SUCCEEDED(hr)) {
        result->rgba.resize(static_cast<size_t>(result->width) * result->height * 4);
        hr = converter->CopyPixels(nullptr, result->width * 4, static_cast<UINT>(result->rgba.size()),
                                   result->rgba.data());
    }
    if (FAILED(hr))
        result->error = std::format("no map image ({:#x})", static_cast<unsigned>(hr));

    SafeRelease(converter);
    SafeRelease(frame);
    SafeRelease(decoder);
    SafeRelease(factory);
    if (SUCCEEDED(coInit))
        CoUninitialize();

    // Without an .ini, assume the image spans the terrain centred on the origin at 2 px per metre.
    const float defaultSpan = result->width ? result->width * 0.5f : 2016.0f;
    result->rect.xLeft = ReadIniFloat(ini, L"x_left", -defaultSpan * 0.5f);
    result->rect.zTop = ReadIniFloat(ini, L"z_top", defaultSpan * 0.5f);
    result->rect.xSpan = ReadIniFloat(ini, L"x_span", defaultSpan);
    result->rect.zSpan = ReadIniFloat(ini, L"z_span", -defaultSpan);

    std::lock_guard lock(*mutex);
    *slot = std::move(result);
}

void MapTexture::RequestLevel(const std::string& level)
{
    if (level == m_level)
        return;
    m_level = level;
    ++m_generation;
    SafeRelease(m_srv);
    SafeRelease(m_texture);
    if (level.empty() || level == "level_main_menu") {
        m_status = "no level";
        return;
    }
    m_status = "loading " + level;
    std::thread(Decode, level, m_suffix, m_generation, m_pendingMutex, m_pending).detach();
}

void MapTexture::Update(ID3D11Device* device, ID3D11DeviceContext* context)
{
    std::unique_ptr<Decoded> decoded;
    {
        std::lock_guard lock(*m_pendingMutex);
        if (!*m_pending)
            return;
        decoded = std::move(*m_pending);
    }
    if (decoded->generation != m_generation)
        return; // the player already moved on to another level
    if (!decoded->error.empty()) {
        m_status = m_level + ": " + decoded->error;
        logger::Warn("Map for {}: {}", m_level, decoded->error);
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = decoded->width;
    desc.Height = decoded->height;
    desc.MipLevels = 0; // full chain, filled by GenerateMips
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_texture)) ||
        FAILED(device->CreateShaderResourceView(m_texture, nullptr, &m_srv))) {
        SafeRelease(m_texture);
        m_status = m_level + ": texture creation failed";
        logger::Error("Map texture creation failed for {} ({}x{})", m_level, decoded->width, decoded->height);
        return;
    }
    context->UpdateSubresource(m_texture, 0, nullptr, decoded->rgba.data(), decoded->width * 4, 0);
    context->GenerateMips(m_srv);
    m_rect = decoded->rect;
    m_status = std::format("{}{}: {}x{}", m_level, m_suffix, decoded->width, decoded->height);
    logger::Info("Map loaded: {}{} {}x{} rect=({}, {}, {}, {})", m_level, m_suffix, decoded->width, decoded->height,
                 m_rect.xLeft, m_rect.zTop, m_rect.xSpan, m_rect.zSpan);
}

void MapTexture::Release()
{
    SafeRelease(m_srv);
    SafeRelease(m_texture);
    m_level.clear();
    ++m_generation;
}

} // namespace render
