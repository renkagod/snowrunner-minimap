#pragma once

namespace hooks {

// Hooks IDXGISwapChain::Present/Present1 through the vtable of a throwaway swap chain.
// Every presented frame is handed to overlay::OnPresent before the real Present runs.
bool InstallPresentHook();

} // namespace hooks
