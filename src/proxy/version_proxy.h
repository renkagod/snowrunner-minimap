#pragma once

namespace proxy {

// Loads System32\version.dll and fills the jump table used by version_stubs.asm.
// Must succeed, otherwise the game's own version.dll calls would crash.
bool LoadRealVersionDll();

} // namespace proxy
