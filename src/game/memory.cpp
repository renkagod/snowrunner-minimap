#include "game/memory.h"

#include "core/log.h"

#include <windows.h>

#include <cstring>

namespace mem {

static Image g_game;

const Image& Game()
{
    return g_game;
}

bool InitGameImage()
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);

    g_game.base = base;
    g_game.whole = {base, base + nt->OptionalHeader.SizeOfImage};

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char name[9] = {};
        std::memcpy(name, section->Name, 8);
        const Range r{base + section->VirtualAddress, base + section->VirtualAddress + section->Misc.VirtualSize};
        if (std::strcmp(name, ".rdata") == 0)
            g_game.rdata = r;
        else if (std::strcmp(name, ".data") == 0)
            g_game.data = r;
    }

    logger::Info("Game image base={:#x} size={:#x} .rdata={:#x}+{:#x} .data={:#x}+{:#x}", base,
                 g_game.whole.end - base, g_game.rdata.begin - base, g_game.rdata.end - g_game.rdata.begin,
                 g_game.data.begin - base, g_game.data.end - g_game.data.begin);
    return g_game.rdata.begin && g_game.data.begin;
}

bool SafeCopy(void* dst, uintptr_t src, size_t size)
{
    if (src < 0x10000 || src + size < src)
        return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsPlausibleHeapPtr(uintptr_t p)
{
    return p >= 0x10000 && p < 0x00007FFFFFFF0000ull && (p & 7) == 0 && !g_game.whole.Contains(p);
}

namespace {

#pragma pack(push, 4)
struct CompleteObjectLocator {
    uint32_t signature; // 1 on x64 (image-relative offsets)
    uint32_t offset;    // offset of this vtable inside the complete object
    uint32_t cdOffset;
    int32_t typeDescriptorRva;
    int32_t classDescriptorRva;
    int32_t selfRva;
};
#pragma pack(pop)

} // namespace

std::vector<uintptr_t> FindVtablesForType(std::string_view decoratedName)
{
    std::vector<uintptr_t> result;
    const Image& img = g_game;

    // TypeDescriptor = { vftable ptr, spare ptr, char name[] } and lives in .data.
    uintptr_t typeDescriptor = 0;
    for (uintptr_t p = img.data.begin; p + decoratedName.size() + 1 <= img.data.end; ++p) {
        if (std::memcmp(reinterpret_cast<const void*>(p), decoratedName.data(), decoratedName.size()) == 0 &&
            *reinterpret_cast<const char*>(p + decoratedName.size()) == '\0') {
            typeDescriptor = p - 16;
            break;
        }
    }
    if (!typeDescriptor)
        return result;
    const auto tdRva = static_cast<int32_t>(typeDescriptor - img.base);

    // Locators live in .rdata and reference themselves through selfRva.
    for (uintptr_t p = img.rdata.begin; p + sizeof(CompleteObjectLocator) <= img.rdata.end; p += 4) {
        const auto* col = reinterpret_cast<const CompleteObjectLocator*>(p);
        if (col->signature != 1 || col->typeDescriptorRva != tdRva || col->offset != 0 ||
            col->selfRva != static_cast<int32_t>(p - img.base))
            continue;
        // The vtable is preceded by a pointer to its locator.
        for (uintptr_t q = img.rdata.begin; q + 8 <= img.rdata.end; q += 8) {
            if (*reinterpret_cast<const uintptr_t*>(q) == p)
                result.push_back(q + 8);
        }
    }
    return result;
}

} // namespace mem
