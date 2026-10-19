#pragma once

#include <string>

namespace config {

enum class Anchor : int { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

struct Settings {
    // [minimap]
    bool enabled = true;
    Anchor anchor = Anchor::BottomRight;
    int marginX = 32;
    int marginY = 32;
    int sizePx = 280;          // diameter on screen
    float metersAcross = 400;  // world distance covered by the diameter
    bool rotateWithTruck = true;
    float opacity = 0.92f;
    bool mapScreenLook = true; // shade like the game's map screen (M) when its textures are captured
    float mapExposure = 1.47f; // the map screen's adapted exposure, as captured

    // [keys] Windows virtual-key codes
    int keyToggle = 0x78;  // F9
    int keyDebug = 0x77;   // F8
    int keyZoomIn = 0xBB;  // '=' / '+'
    int keyZoomOut = 0xBD; // '-'

    // [calibration] fixes for axis conventions until they are confirmed in game
    float headingOffsetDeg = 0;
    bool headingMirror = false;

    // [debug] full path to renderdoc.dll; empty keeps RenderDoc out of the game
    std::wstring renderdocDll;
};

Settings& Get();
void Load();
void Save();

} // namespace config
