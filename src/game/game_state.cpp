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
            if (level != g_level && !level.empty())
                logger::Info("Level changed: '{}'", level);
            g_level = level;
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
