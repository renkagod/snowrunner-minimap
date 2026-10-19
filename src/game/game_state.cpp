#include "game/game_state.h"

#include "core/config.h"
#include "core/log.h"
#include "game/memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

namespace game {

// Layout found by the SnowMap project (ElAmine01) on the 2026 builds:
// [TRUCK_CONTROL + 0x08] -> sim node, [+0x60] -> intermediate, [+0x68] -> chassis body.
namespace layout {
constexpr uintptr_t kTruckSimNode = 0x08;
constexpr uintptr_t kTruckInputBlocked = 0x2C; // byte, set while the pause menu, map or another screen is up
constexpr uintptr_t kIntermediate = 0x60;
constexpr uintptr_t kChassisBody = 0x68;
constexpr uintptr_t kBodyAxes = 0x90; // three float4 basis vectors
constexpr uintptr_t kBodyPosition = 0xC0;
constexpr uintptr_t kSessionLevelLength = 0x10;
constexpr uintptr_t kSessionLevelChars = 0x18;

// GAME_LOGIC caches the current level's data at +0x690 (sub_a9a7b0 returns it; the minimap init
// sub_8757e0 builds the map screen from it). Its fields, as sub_8d4760 and sub_8d6c80 read them:
constexpr uintptr_t kLevelData = 0x690;
constexpr uintptr_t kLevelMaxHeight = 0x00; // float
constexpr uintptr_t kLevelName = 0x08;      // engine string
constexpr uintptr_t kLevelGroups = 0x30;    // std::vector of 40-byte instance groups
constexpr size_t kGroupSize = 40;
constexpr uintptr_t kGroupName = 0x00;      // engine string
constexpr uintptr_t kGroupInstances = 0x18; // PackedInstance*
constexpr uintptr_t kGroupCount = 0x20;     // int

// Fog of war, one object per level of the region (FogOfWar.cpp; the minimap's update binding
// sub_9b4250 hands +0x08 to sub_8d5170, which uploads it as the map screen's g_txCloaking).
constexpr uintptr_t kFogCells = 0x08;  // u8*, then u32 width, u32 height
constexpr uintptr_t kFogWidth = 0x10;
constexpr uintptr_t kFogHeight = 0x14;
constexpr uintptr_t kFogLevel = 0x28;  // engine string
} // namespace layout

static std::atomic<uintptr_t> g_levelSlot{0};
static std::atomic<uintptr_t> g_truckSlot{0};
static std::vector<uintptr_t> g_truckVtables; // written once by the scanner thread, read only there
static std::atomic<size_t> g_truckVtableCount{0};
static std::atomic<int> g_levelScans{0};
static std::atomic<int> g_truckScans{0};

static std::mutex g_levelMutex;
static std::string g_level;

static std::atomic<unsigned> g_heightMapSize{0};
static std::atomic<float> g_maxHeight{0};

static std::vector<uintptr_t> g_gameLogicVtables; // scanner thread only
static std::atomic<uintptr_t> g_gameLogicSlot{0};
static std::mutex g_navMutex;
static std::shared_ptr<const LevelNavData> g_navData;

static std::atomic<uintptr_t> g_fogObject{0};

// The terrain shaders get float4 g_vTerrainHeightmapParams = (max height, 1/size, 1/size, 0).
// Copies of that constant buffer sit in ordinary read-write memory.
static const uint8_t* FindHeightParams(const uint8_t* begin, size_t size, uint32_t inverseBits)
{
    __try {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(begin);
        const uint32_t* end = p + size / 4;
        for (++p; p + 2 < end; ++p) {
            if (p[0] == inverseBits && p[1] == inverseBits && p[2] == 0)
                return reinterpret_cast<const uint8_t*>(p - 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

static float ScanForMaxHeight(unsigned heightMapSize)
{
    const float inverse = 1.0f / static_cast<float>(heightMapSize);
    uint32_t inverseBits;
    std::memcpy(&inverseBits, &inverse, 4);
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t address = 0x10000; VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi));
         address = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Protect != PAGE_READWRITE || mbi.Type == MEM_IMAGE)
            continue;
        const auto* begin = static_cast<const uint8_t*>(mbi.BaseAddress);
        size_t left = mbi.RegionSize;
        while (left >= 16) {
            const uint8_t* hit = FindHeightParams(begin, left, inverseBits);
            if (!hit)
                break;
            float height = 0;
            if (mem::Read(reinterpret_cast<uintptr_t>(hit), height) && height > 1.0f && height < 10000.0f)
                return height;
            left -= static_cast<size_t>(hit + 4 - begin);
            begin = hit + 4;
        }
    }
    return 0;
}

static bool ReadLevelName(uintptr_t slot, std::string& out)
{
    uintptr_t session = 0;
    if (!mem::Read(slot, session) || !mem::IsPlausibleHeapPtr(session))
        return false;
    char buf[0x28];
    if (!mem::SafeCopy(buf, session + layout::kSessionLevelLength, sizeof(buf)))
        return false;
    uint32_t len;
    std::memcpy(&len, buf, sizeof(len));
    const char* chars = buf + (layout::kSessionLevelChars - layout::kSessionLevelLength);
    if (len < 6 || len > 31 || std::memcmp(chars, "level_", 6) != 0 || chars[len] != '\0' ||
        std::strlen(chars) != len)
        return false;
    out.assign(chars, len);
    return true;
}

static uintptr_t ScanForLevelSlot()
{
    const mem::Range data = mem::Game().data;
    std::string level;
    for (uintptr_t slot = data.begin; slot + 8 <= data.end; slot += 8) {
        if (ReadLevelName(slot, level))
            return slot;
    }
    return 0;
}

static bool HasVtable(uintptr_t object, const std::vector<uintptr_t>& vtables)
{
    uintptr_t vtable = 0;
    if (!mem::Read(object, vtable))
        return false;
    for (uintptr_t v : vtables) {
        if (v == vtable)
            return true;
    }
    return false;
}

static bool IsTruckControl(uintptr_t object)
{
    return HasVtable(object, g_truckVtables);
}

// Static pointer in the exe's .data that points at an object of one of the given types.
static uintptr_t ScanForSlot(const std::vector<uintptr_t>& vtables)
{
    const mem::Range data = mem::Game().data;
    for (uintptr_t slot = data.begin; slot + 8 <= data.end; slot += 8) {
        const uintptr_t value = *reinterpret_cast<const uintptr_t*>(slot);
        if (mem::IsPlausibleHeapPtr(value) && HasVtable(value, vtables))
            return slot;
    }
    return 0;
}

// The engine's string: u32 length, u32 "stored inline" flag, then the characters inline or a pointer.
static bool ReadEngineString(uintptr_t address, std::string& out)
{
    uint32_t header[2];
    if (!mem::SafeCopy(header, address, sizeof(header)) || header[0] == 0 || header[0] > 255)
        return false;
    uintptr_t chars = address + 8;
    if (!header[1] && (!mem::Read(chars, chars) || !mem::IsPlausibleHeapPtr(chars)))
        return false;
    out.resize(header[0]);
    return mem::SafeCopy(out.data(), chars, header[0]);
}

// Copies the map-screen model placement of the given level out of GAME_LOGIC's level data cache.
// Returns null while the cache holds another level (or nothing yet).
static std::shared_ptr<LevelNavData> ReadLevelNavData(uintptr_t gameLogic, const std::string& level)
{
    const uintptr_t data = gameLogic + layout::kLevelData;
    auto out = std::make_shared<LevelNavData>();
    if (!ReadEngineString(data + layout::kLevelName, out->level) || out->level != level ||
        !mem::Read(data + layout::kLevelMaxHeight, out->maxHeight))
        return nullptr;
    uintptr_t begin = 0, end = 0;
    if (!mem::Read(data + layout::kLevelGroups, begin) || !mem::Read(data + layout::kLevelGroups + 8, end) ||
        end < begin || (end - begin) % layout::kGroupSize != 0 || (end - begin) / layout::kGroupSize > 100000)
        return nullptr;
    for (uintptr_t group = begin; group < end; group += layout::kGroupSize) {
        NavInstanceGroup g;
        uintptr_t instances = 0;
        int32_t count = 0;
        if (!ReadEngineString(group + layout::kGroupName, g.model) ||
            !mem::Read(group + layout::kGroupInstances, instances) || !mem::Read(group + layout::kGroupCount, count) ||
            count < 0 || count > 10'000'000)
            return nullptr;
        if (count == 0)
            continue;
        g.instances.resize(static_cast<size_t>(count));
        if (!mem::SafeCopy(g.instances.data(), instances, g.instances.size() * sizeof(PackedInstance)))
            return nullptr;
        out->instanceCount += g.instances.size();
        out->groups.push_back(std::move(g));
    }
    return out;
}

// Reads a fog object's cells if it looks like the fog of war of the given level.
static bool ReadFogObject(uintptr_t object, const std::string& level, std::vector<uint8_t>& cells, unsigned& width,
                          unsigned& height)
{
    std::string name;
    uintptr_t data = 0;
    uint32_t w = 0, h = 0;
    if (!ReadEngineString(object + layout::kFogLevel, name) || name != level ||
        !mem::Read(object + layout::kFogCells, data) || !mem::IsPlausibleHeapPtr(data) ||
        !mem::Read(object + layout::kFogWidth, w) || !mem::Read(object + layout::kFogHeight, h) || w < 16 || h < 16 ||
        w > 4096 || h > 4096)
        return false;
    cells.resize(size_t(w) * h);
    if (!mem::SafeCopy(cells.data(), data, cells.size()))
        return false;
    width = w;
    height = h;
    return true;
}

// First occurrence of a byte pattern, or null; tolerates pages vanishing under it.
static const uint8_t* FindBytes(const uint8_t* begin, size_t size, const uint8_t* pattern, size_t length)
{
    __try {
        for (const uint8_t* p = begin; p + length <= begin + size; ++p) {
            p = static_cast<const uint8_t*>(std::memchr(p, pattern[0], begin + size - p));
            if (!p || p + length > begin + size)
                return nullptr;
            if (std::memcmp(p, pattern, length) == 0)
                return p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// The level's fog object is found by its level name, stored inline as an engine string at +0x28.
static uintptr_t FindFogOfWar(const std::string& level)
{
    std::vector<uint8_t> pattern(8 + level.size());
    const uint32_t header[2] = {static_cast<uint32_t>(level.size()), 1};
    std::memcpy(pattern.data(), header, 8);
    std::memcpy(pattern.data() + 8, level.data(), level.size());

    std::vector<uint8_t> cells;
    unsigned w, h;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t address = 0x10000; VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi));
         address = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Protect != PAGE_READWRITE || mbi.Type == MEM_IMAGE)
            continue;
        const auto* begin = static_cast<const uint8_t*>(mbi.BaseAddress);
        const auto* end = begin + mbi.RegionSize;
        for (const uint8_t* hit = begin; (hit = FindBytes(hit, end - hit, pattern.data(), pattern.size())); ++hit) {
            const uintptr_t object = reinterpret_cast<uintptr_t>(hit) - layout::kFogLevel;
            if (!ReadFogObject(object, level, cells, w, h))
                continue;
            // Fog cells only ever hold these three values; anything else is a false match.
            if (std::all_of(cells.begin(), cells.end(), [](uint8_t c) { return c == 0 || c == 128 || c == 255; }))
                return object;
        }
    }
    return 0;
}

static void ScannerThread()
{
    g_truckVtables = mem::FindVtablesForType(".?AVTRUCK_CONTROL@combine@@");
    g_truckVtableCount = g_truckVtables.size();
    logger::Info("TRUCK_CONTROL vtables found: {}", g_truckVtables.size());
    for (uintptr_t v : g_truckVtables)
        logger::Info("  vtable at exe+{:#x}", v - mem::Game().base);
    g_gameLogicVtables = mem::FindVtablesForType(".?AVGAME_LOGIC@combine@@");
    logger::Info("GAME_LOGIC vtables found: {}", g_gameLogicVtables.size());
    std::string navLevel; // level whose model placement has been published

    int levelMisses = 0;
    for (;;) {
        // Level name. The session global stays valid in menus, so a few misses mean it moved.
        std::string level;
        uintptr_t slot = g_levelSlot.load();
        if (slot && !ReadLevelName(slot, level) && ++levelMisses > 3) {
            g_levelSlot = slot = 0;
            levelMisses = 0;
        }
        if (!slot) {
            ++g_levelScans;
            slot = ScanForLevelSlot();
            g_levelSlot = slot;
            if (slot) {
                ReadLevelName(slot, level);
                logger::Info("Game session slot exe+{:#x}, level '{}'", slot - mem::Game().base, level);
            }
        }
        if (!level.empty())
            levelMisses = 0;
        {
            std::lock_guard lock(g_levelMutex);
            if (level != g_level && !level.empty()) {
                logger::Info("Level changed: '{}'", level);
                g_maxHeight = 0;
            }
            g_level = level;
        }

        // Map-screen model placement, read once per level from GAME_LOGIC's level data cache.
        const bool playable = !level.empty() && level != "level_main_menu";
        uintptr_t gameLogicSlot = g_gameLogicSlot.load();
        uintptr_t gameLogic = 0;
        if (gameLogicSlot && (!mem::Read(gameLogicSlot, gameLogic) || (gameLogic && !HasVtable(gameLogic, g_gameLogicVtables))))
            g_gameLogicSlot = gameLogicSlot = gameLogic = 0;
        if (!gameLogicSlot && playable && !g_gameLogicVtables.empty()) {
            gameLogicSlot = ScanForSlot(g_gameLogicVtables);
            g_gameLogicSlot = gameLogicSlot;
            if (gameLogicSlot) {
                mem::Read(gameLogicSlot, gameLogic);
                logger::Info("GAME_LOGIC slot exe+{:#x}", gameLogicSlot - mem::Game().base);
            }
        }
        if (playable && level != navLevel && gameLogic) {
            if (auto nav = ReadLevelNavData(gameLogic, level)) {
                logger::Info("Map models of '{}': {} groups, {} instances, max height {}", level, nav->groups.size(),
                             nav->instanceCount, nav->maxHeight);
                navLevel = level;
                std::lock_guard lock(g_navMutex);
                g_navData = std::move(nav);
            }
        }

        // The level's fog of war object; looked for again when the level changes or it goes away.
        static std::string fogLevel;
        static int fogRetry = 0;
        if (playable && (level != fogLevel || !g_fogObject.load()) && fogRetry-- <= 0) {
            g_fogObject = 0;
            const ULONGLONG start = GetTickCount64();
            const uintptr_t fog = FindFogOfWar(level);
            if (fog || level != fogLevel)
                logger::Info("Fog of war of '{}': {} (scan {} ms)", level,
                             fog ? std::format("{:#x}", fog) : std::string("not found"), GetTickCount64() - start);
            fogLevel = level;
            fogRetry = fog ? 0 : 5;
            g_fogObject = fog;
        }

        // Terrain height scale, retried every few seconds until the terrain constants show up.
        static int heightRetry = 0;
        const unsigned heightMapSize = g_heightMapSize.load();
        if (heightMapSize && g_maxHeight.load() == 0 && heightRetry-- <= 0) {
            const ULONGLONG start = GetTickCount64();
            const float height = ScanForMaxHeight(heightMapSize);
            g_maxHeight = height;
            heightRetry = 5;
            logger::Info("Terrain max height {} (height map {}, scan {} ms)", height, heightMapSize,
                         GetTickCount64() - start);
        }

        // Truck control. Its global is null outside a level, so only scan while one is loaded.
        uintptr_t truckSlot = g_truckSlot.load();
        if (truckSlot) {
            uintptr_t value = 0;
            if (mem::Read(truckSlot, value) && value && !IsTruckControl(value)) {
                logger::Warn("Truck slot no longer points at TRUCK_CONTROL, rescanning");
                g_truckSlot = truckSlot = 0;
            }
        }
        if (!truckSlot && !g_truckVtables.empty() && !level.empty() && level != "level_main_menu") {
            ++g_truckScans;
            truckSlot = ScanForSlot(g_truckVtables);
            if (truckSlot) {
                g_truckSlot = truckSlot;
                logger::Info("TRUCK_CONTROL slot exe+{:#x}", truckSlot - mem::Game().base);
            }
        }

        Sleep(1000);
    }
}

void Start()
{
    std::thread(ScannerThread).detach();
}

std::string CurrentLevel()
{
    std::lock_guard lock(g_levelMutex);
    return g_level;
}

static bool Finite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

TruckPose ReadTruck()
{
    TruckPose pose;
    const uintptr_t slot = g_truckSlot.load();
    uintptr_t control = 0, simNode = 0, intermediate = 0, body = 0;
    if (!slot || !mem::Read(slot, control) || !mem::IsPlausibleHeapPtr(control) ||
        !mem::Read(control + layout::kTruckSimNode, simNode) || !mem::IsPlausibleHeapPtr(simNode) ||
        !mem::Read(simNode + layout::kIntermediate, intermediate) || !mem::IsPlausibleHeapPtr(intermediate) ||
        !mem::Read(intermediate + layout::kChassisBody, body) || !mem::IsPlausibleHeapPtr(body))
        return pose;

    float raw[16];
    if (!mem::SafeCopy(raw, body + layout::kBodyAxes, sizeof(raw)))
        return pose;
    for (int i = 0; i < 3; ++i)
        pose.axis[i] = {raw[i * 4 + 0], raw[i * 4 + 1], raw[i * 4 + 2]};
    pose.position = {raw[12], raw[13], raw[14]};
    pose.chassisBody = body;

    // Checked in game by driving straight: +0x90 is forward, +0xA0 up, +0xB0 left.
    const Vec3& forward = pose.axis[0];
    const float len = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
    if (!Finite(pose.position) || !Finite(forward) || len < 0.9f || len > 1.1f ||
        std::fabs(pose.position.x) > 20000 || std::fabs(pose.position.z) > 20000)
        return pose;

    const config::Settings& cfg = config::Get();
    float heading = std::atan2(forward.x, forward.z);
    if (cfg.headingMirror)
        heading = -heading;
    pose.headingRad = heading + cfg.headingOffsetDeg * 3.14159265f / 180.0f;
    pose.valid = true;
    return pose;
}

void SetHeightMapSize(unsigned size)
{
    if (g_heightMapSize.exchange(size) != size)
        g_maxHeight = 0;
}

float TerrainMaxHeight()
{
    // The level data cache holds it directly; the constant-buffer scan covers builds where it moved.
    if (const auto nav = GetLevelNavData(); nav && nav->level == CurrentLevel() && nav->maxHeight > 1.0f &&
                                            nav->maxHeight < 10000.0f)
        return nav->maxHeight;
    return g_maxHeight.load();
}

std::shared_ptr<const LevelNavData> GetLevelNavData()
{
    std::lock_guard lock(g_navMutex);
    return g_navData;
}

bool ReadFogOfWar(std::vector<uint8_t>& cells, unsigned& width, unsigned& height)
{
    uintptr_t object = g_fogObject.load();
    if (!object)
        return false;
    if (ReadFogObject(object, CurrentLevel(), cells, width, height))
        return true;
    g_fogObject.compare_exchange_strong(object, 0); // the scanner looks for it again
    return false;
}

bool IsScreenOpen()
{
    const uintptr_t slot = g_truckSlot.load();
    uintptr_t control = 0;
    uint8_t blocked = 0;
    return slot && mem::Read(slot, control) && mem::IsPlausibleHeapPtr(control) &&
           mem::Read(control + layout::kTruckInputBlocked, blocked) && blocked != 0;
}

ScanStatus GetStatus()
{
    ScanStatus s;
    s.level = CurrentLevel();
    s.levelSlot = g_levelSlot;
    s.truckSlot = g_truckSlot;
    s.gameLogicSlot = g_gameLogicSlot;
    s.truckVtables = g_truckVtableCount;
    s.levelScans = g_levelScans;
    s.truckScans = g_truckScans;
    return s;
}

} // namespace game
