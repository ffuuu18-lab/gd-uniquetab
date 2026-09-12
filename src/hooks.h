// hooks.h - MinHook detours on the four engine entry points this prototype needs.
#pragma once

namespace ut {

// MH_Initialize + MH_CreateHook + MH_EnableHook for every resolved target. Logs each step.
bool hooksInstall();

// MH_DisableHook(MH_ALL_HOOKS) + MH_Uninitialize.
void hooksRemove();

// Counters the worker thread logs, so the smoke test can assert on them.
unsigned long long hookFrameCount();
unsigned long long hookUpdateCount();
bool hookSawTransferOpen();

}  // namespace ut
