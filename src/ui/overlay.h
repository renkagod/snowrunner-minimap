#pragma once

struct IDXGISwapChain;

namespace overlay {

// Called from the Present hook on the game's render thread, before the frame is shown.
void OnPresent(IDXGISwapChain* swapChain);

} // namespace overlay
