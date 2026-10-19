#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

// One packed instance as the map screen's ModelNavigation shader takes it (28 bytes): world position
// and a shade seed (w), an RGBA8 quaternion, an RGBA8 whose z/w hold the scale, and an unused RGBA8.
struct PackedInstance {
    float x, y, z, w;
    uint8_t rotation[4];
    uint8_t scale[4];
    uint8_t colour[4];
};
static_assert(sizeof(PackedInstance) == 28);

// The map screen's model placement for one landmark model, e.g. "landmarks/spruce_01_lmk".
struct NavInstanceGroup {
    std::string model;
    std::vector<PackedInstance> instances;
};

// What the engine builds the map screen's models from: its per-level data cache (GAME_LOGIC+0x690),
// filled when the level loads, long before the map screen is first opened.
struct LevelNavData {
    std::string level;
    float maxHeight = 0; // height that the terrain height map's full range stands for
    std::vector<NavInstanceGroup> groups;
    size_t instanceCount = 0;
};

struct ScanStatus {
    std::string level;
    uintptr_t levelSlot = 0;   // static pointer to the game session
    uintptr_t truckSlot = 0;   // static pointer to combine::TRUCK_CONTROL
    uintptr_t gameLogicSlot = 0; // static pointer to combine::GAME_LOGIC
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

// The current level's map-screen model placement, null until it has been read. A new object is
// published when the level changes, so comparing pointers tells when to rebuild.
std::shared_ptr<const LevelNavData> GetLevelNavData();

// The current level's fog of war, live, as the map screen shows it: one byte per cell, 0 hidden,
// 128 explored, 255 revealed; row 0 is the south edge. False until the engine's fog object is found.
bool ReadFogOfWar(std::vector<uint8_t>& cells, unsigned& width, unsigned& height);

ScanStatus GetStatus();

} // namespace game
