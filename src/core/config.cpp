#include "core/config.h"

#include "core/log.h"
#include "core/paths.h"

#include <windows.h>

#include <algorithm>
#include <string>

namespace config {

static Settings g_settings;

static std::wstring IniPath()
{
    return (paths::ModDir() / L"SnowMinimap.ini").wstring();
}

static int ReadInt(const wchar_t* section, const wchar_t* key, int fallback)
{
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, IniPath().c_str()));
}

static float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, IniPath().c_str());
    if (!buf[0])
        return fallback;
    wchar_t* end = nullptr;
    const float value = std::wcstof(buf, &end);
    return end != buf ? value : fallback;
}

static void WriteInt(const wchar_t* section, const wchar_t* key, int value)
{
    WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), IniPath().c_str());
}

static void WriteFloat(const wchar_t* section, const wchar_t* key, float value)
{
    wchar_t buf[64];
    swprintf_s(buf, L"%g", value);
    WritePrivateProfileStringW(section, key, buf, IniPath().c_str());
}

Settings& Get()
{
    return g_settings;
}

void Load()
{
    const bool exists = GetFileAttributesW(IniPath().c_str()) != INVALID_FILE_ATTRIBUTES;
    Settings d;
    Settings& s = g_settings;

    s.enabled = ReadInt(L"minimap", L"enabled", d.enabled) != 0;
    s.anchor = static_cast<Anchor>(std::clamp(ReadInt(L"minimap", L"anchor", static_cast<int>(d.anchor)), 0, 3));
    s.marginX = ReadInt(L"minimap", L"margin_x", d.marginX);
    s.marginY = ReadInt(L"minimap", L"margin_y", d.marginY);
    s.sizePx = std::clamp(ReadInt(L"minimap", L"size_px", d.sizePx), 64, 2048);
    s.metersAcross = std::clamp(ReadFloat(L"minimap", L"meters_across", d.metersAcross), 25.0f, 4000.0f);
    s.rotateWithTruck = ReadInt(L"minimap", L"rotate_with_truck", d.rotateWithTruck) != 0;
    s.opacity = std::clamp(ReadFloat(L"minimap", L"opacity", d.opacity), 0.1f, 1.0f);
    s.mapScreenLook = ReadInt(L"minimap", L"map_screen_look", d.mapScreenLook) != 0;
    s.mapExposure = std::clamp(ReadFloat(L"minimap", L"map_exposure", d.mapExposure), 0.1f, 8.0f);
    s.mapModels = ReadInt(L"minimap", L"map_models", d.mapModels) != 0;

    s.keyToggle = ReadInt(L"keys", L"toggle", d.keyToggle);
    s.keyDebug = ReadInt(L"keys", L"debug", d.keyDebug);
    s.keyZoomIn = ReadInt(L"keys", L"zoom_in", d.keyZoomIn);
    s.keyZoomOut = ReadInt(L"keys", L"zoom_out", d.keyZoomOut);

    s.headingOffsetDeg = ReadFloat(L"calibration", L"heading_offset_deg", d.headingOffsetDeg);
    s.headingMirror = ReadInt(L"calibration", L"heading_mirror", d.headingMirror) != 0;

    wchar_t path[MAX_PATH];
    GetPrivateProfileStringW(L"debug", L"renderdoc_dll", L"", path, MAX_PATH, IniPath().c_str());
    s.renderdocDll = path;

    if (!exists)
        Save(); // give the player a file with every option spelled out
    logger::Info("Config loaded ({})", exists ? "existing file" : "defaults written");
}

void Save()
{
    const Settings& s = g_settings;
    WriteInt(L"minimap", L"enabled", s.enabled);
    WriteInt(L"minimap", L"anchor", static_cast<int>(s.anchor));
    WriteInt(L"minimap", L"margin_x", s.marginX);
    WriteInt(L"minimap", L"margin_y", s.marginY);
    WriteInt(L"minimap", L"size_px", s.sizePx);
    WriteFloat(L"minimap", L"meters_across", s.metersAcross);
    WriteInt(L"minimap", L"rotate_with_truck", s.rotateWithTruck);
    WriteFloat(L"minimap", L"opacity", s.opacity);
    WriteInt(L"minimap", L"map_screen_look", s.mapScreenLook);
    WriteFloat(L"minimap", L"map_exposure", s.mapExposure);
    WriteInt(L"minimap", L"map_models", s.mapModels);

    WriteInt(L"keys", L"toggle", s.keyToggle);
    WriteInt(L"keys", L"debug", s.keyDebug);
    WriteInt(L"keys", L"zoom_in", s.keyZoomIn);
    WriteInt(L"keys", L"zoom_out", s.keyZoomOut);

    WriteFloat(L"calibration", L"heading_offset_deg", s.headingOffsetDeg);
    WriteInt(L"calibration", L"heading_mirror", s.headingMirror);

    WritePrivateProfileStringW(L"debug", L"renderdoc_dll", s.renderdocDll.c_str(), IniPath().c_str());
}

} // namespace config
