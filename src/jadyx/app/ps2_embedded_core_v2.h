#pragma once
// C ABI for the real PCSX2/XBSX2 engine compiled into the *same* Xbox APPX.
// This file is intentionally usable by both C++/CX Hub and C++/WinRT PCSX2.
// WinRT swapchain panel must be supplied on the XAML UI thread.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define UAIBRAIN_PS2_CORE_ABI_V2 2u
typedef enum UaiBrainPs2BootResultV2 {
 UAIBRAIN_PS2_V2_BOOT_ACCEPTED=0, // queued on CPU thread, not a gameplay claim
 UAIBRAIN_PS2_V2_NO_CORE=1,
 UAIBRAIN_PS2_V2_UNSUPPORTED=2,
 UAIBRAIN_PS2_V2_INVALID_GAME=3,
 UAIBRAIN_PS2_V2_INTERNAL_ERROR=4
} UaiBrainPs2BootResultV2;
typedef struct UaiBrainPs2CoreApiV2 {
 uint32_t abi_version;
 uint32_t struct_size;
 // Receives a borrowed ABI IUnknown* implementing ISwapChainPanelNative.
 // The frontend keeps the SwapChainPanel alive until the engine is stopped.
 // Must be called on the XAML UI thread. Initializes host and CPU thread.
 int (*attach_xaml_surface)(void* panel_abi, uint32_t width, uint32_t height);
 int (*is_ready)(void);
 // Nonblocking: enqueues one disc for the existing CPU thread.
 UaiBrainPs2BootResultV2 (*boot_disc)(
  const wchar_t* absolute_game_path,
  const wchar_t* writable_save_root,
  int request_auto_resume);
 // Called on UI thread periodically; never pumps the CoreDispatcher itself.
 void (*poll_ui)(void);
 // Returns 1 when an active VM exists, 0 when stopped, -1 on error.
 int (*has_active_vm)(void);
 // Asynchronous request to stop VM, saving state when possible.
 void (*request_exit)(void);
} UaiBrainPs2CoreApiV2;
// The only exported symbol of the embedded PCSX2 DLL. The MSVC linkage and
// storage attribute must be declared ONCE, before the function definition.
// The Hub's no-core stub and consumer see a plain C ABI declaration.
#if defined(_WIN32) && defined(UAIBRAIN_PS2_CORE_PROVIDER_BUILD)
#define UAIBRAIN_PS2_CORE_ENTRY __declspec(dllexport)
#else
#define UAIBRAIN_PS2_CORE_ENTRY
#endif
UAIBRAIN_PS2_CORE_ENTRY const UaiBrainPs2CoreApiV2* uaibrain_ps2_core_api_v2(void);
// Additive native-host exports: the original v2 struct layout is unchanged.
// Idle=0, Starting=1, Active=2 (VM initialized, not first frame), Failed=3,
// Stopped=4. A dead CPU thread or faulted bridge reports Failed.
UAIBRAIN_PS2_CORE_ENTRY int uaibrain_ps2_core_session_state_v2(void);
// UI thread only, after Failed/Stopped/Idle. Joins CPU/GS and detaches host
// callbacks before another engine may acquire the CoreWindow. 1 = released.
UAIBRAIN_PS2_CORE_ENTRY int uaibrain_ps2_core_release_v2(void);
#ifdef __cplusplus
}
#endif
