#include "game/game_state.h"

#include "core/config.h"
#include "core/log.h"
#include "game/memory.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
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

static bool IsTruckControl(uintptr_t object)
{
    uintptr_t vtable = 0;
    if (!mem::Read(object, vtable))
        return false;
    for (uintptr_t v : g_truckVtables) {
        if (v == vtable)
            return true;
    }
    return false;
}

static uintptr_t ScanForTruckSlot()
{
    const mem::Range data = mem::Game().data;
    for (uintptr_t slot = data.begin; slot + 8 <= data.end; slot += 8) {
        const uintptr_t value = *reinterpret_cast<const uintptr_t*>(slot);
        if (mem::IsPlausibleHeapPtr(value) && IsTruckControl(value))
            return slot;
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
            truckSlot = ScanForTruckSlot();
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
    return g_maxHeight.load();
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
    s.truckVtables = g_truckVtableCount;
    s.levelScans = g_levelScans;
    s.truckScans = g_truckScans;
    return s;
}

} // namespace game
