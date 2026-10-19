#pragma once

#include <string>

namespace dev {

// Development aid: loads RenderDoc into the game so frames can be captured without launching
// through RenderDoc (Epic only starts the game via its launcher). Must run before the game
// creates its D3D11 device. Captures go to SnowMinimap\captures, key F12.
void LoadRenderDoc(const std::wstring& dllPath);

// Called once per frame. Creating SnowMinimap\captures\trigger makes the mod capture the next
// frame itself with callstacks forced on, since a target-control client may override options.
void OnFrame();

} // namespace dev
