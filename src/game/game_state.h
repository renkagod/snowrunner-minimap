#pragma once

#include <cstdint>
#include <string>

namespace game {

struct Vec3 {
    float x, y, z;
};

struct TruckPose {
    bool valid = false;
    Vec3 position{};   // world metres; Y is height
    Vec3 axis[3]{};    // chassis basis: forward (+0x90), up (+0xA0), left (+0xB0)
    float headingRad = 0; // compass heading, 0 = +Z, clockwise, calibration applied
    uintptr_t chassisBody = 0;
};

struct ScanStatus {
    std::string level;
    uintptr_t levelSlot = 0;   // static pointer to the game session
    uintptr_t truckSlot = 0;   // static pointer to combine::TRUCK_CONTROL
    size_t truckVtables = 0;
    int levelScans = 0;
    int truckScans = 0;
};

// Starts the background thread that locates the engine globals and tracks the level name.
void Start();

// Current level id such as "level_us_01_01", empty in menus or before the scan succeeds.
std::string CurrentLevel();

// Reads the player's truck right now. Cheap; meant to be called once per frame.
TruckPose ReadTruck();

// Height in metres that the terrain height map's full range stands for (186 on Black River).
// The engine only keeps it in its shader constants, so it is found by scanning memory for them
// once the height map's size is known; 0 until found.
void SetHeightMapSize(unsigned size);
float TerrainMaxHeight();

// True while a game screen (pause menu, map, ...) has taken the truck's input.
bool IsScreenOpen();

ScanStatus GetStatus();

} // namespace game
