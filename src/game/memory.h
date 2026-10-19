#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mem {

struct Range {
    uintptr_t begin = 0;
    uintptr_t end = 0;
    bool Contains(uintptr_t p, size_t size = 1) const { return p >= begin && p + size <= end; }
};

struct Image {
    uintptr_t base = 0;
    Range whole;
    Range rdata;
    Range data;
};

// SnowRunner.exe as mapped in this process.
const Image& Game();
bool InitGameImage();

// Copies memory, returning false instead of crashing on a bad address.
bool SafeCopy(void* dst, uintptr_t src, size_t size);

template <class T>
bool Read(uintptr_t address, T& out)
{
    return SafeCopy(&out, address, sizeof(T));
}

// Heuristic for "looks like a heap pointer": canonical user-space address outside the exe image.
bool IsPlausibleHeapPtr(uintptr_t p);

// MSVC RTTI: every vtable whose complete object locator names the given decorated type,
// e.g. ".?AVTRUCK_CONTROL@combine@@". Only primary vtables (object offset 0) are returned.
std::vector<uintptr_t> FindVtablesForType(std::string_view decoratedName);

} // namespace mem
