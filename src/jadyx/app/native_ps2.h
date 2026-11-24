// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <windows.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Gaming.Input.h>
#include "jadyx/app/boot_session.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/app/ps2_embedded_core_v2.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/play_history.h"
#include "jadyx/storage/storage_path.h"

namespace Jadyx {
inline constexpr int NativePs2Unavailable = 10001;
inline constexpr int NativePs2ReleaseFailed = 10003;

// Opt-in local package only. A marker is not a runtime proof: the actual ABI
// is checked on launch. Never load DLLs from user-writable game folders.
inline bool NativePs2Staged() {
    try {
        const std::filesystem::path root{
            winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()};
        return std::filesystem::is_regular_file(root / L"PS2-Native-Dependencies.json") &&
               std::filesystem::is_regular_file(root / L"uaibrain_ps2_embedded.dll");
    } catch (...) { return false; }
}

inline bool NativePs2Candidate(std::string_view path) {
    return ClassifyMediaPath(path) == MediaKind::PS2Disc && !IsStoragePath(path);
}

// Runs on the owning CoreWindow UI thread. PCSX2 starts its own CPU thread;
// the Jadyx dispatcher remains responsive. No XAML app or second package.
inline int RunNativePs2(const winrt::Windows::UI::Core::CoreWindow& window,
                        const BootSurface& surface, const std::string& path) {
    if (!NativePs2Staged() || !NativePs2Candidate(path)) return NativePs2Unavailable;
    // Keep the module loaded for the process lifetime: WinRT callbacks may
    // still have code addresses in it even after the VM/CPU thread is joined.
    static HMODULE module = LoadPackagedLibrary(L"uaibrain_ps2_embedded.dll", 0);
    if (!module) { WriteDiag("PS2: LoadPackagedLibrary failed"); return NativePs2Unavailable; }
    using GetApi = const UaiBrainPs2CoreApiV2* (*)();
    using Status = int (*)();
    const auto get_api = reinterpret_cast<GetApi>(GetProcAddress(module, "uaibrain_ps2_core_api_v2"));
    const auto state = reinterpret_cast<Status>(GetProcAddress(module, "uaibrain_ps2_core_session_state_v2"));
    const auto release = reinterpret_cast<Status>(GetProcAddress(module, "uaibrain_ps2_core_release_v2"));
    if (!get_api || !state || !release) return NativePs2Unavailable;
    const auto api = get_api();
    if (!api || api->abi_version != UAIBRAIN_PS2_CORE_ABI_V2 ||
        api->struct_size < sizeof(UaiBrainPs2CoreApiV2) || !api->attach_xaml_surface ||
        !api->is_ready || !api->boot_disc || !api->poll_ui || !api->request_exit)
        return NativePs2Unavailable;
    bool attached = false;
    int result = 2;
    try {
        const auto game = winrt::to_hstring(path);
        const auto local = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
        attached = api->attach_xaml_surface(surface.core_window, surface.width, surface.height) != 0;
        if (attached && api->is_ready() &&
            api->boot_disc(game.c_str(), local.c_str(), 0) == UAIBRAIN_PS2_V2_BOOT_ACCEPTED) {
            using namespace winrt::Windows::UI::Core;
            using namespace winrt::Windows::Gaming::Input;
            bool saw_active = false;
            bool exit_requested = false;
            bool chord_released = false;
            const auto chord = GamepadButtons::Menu | GamepadButtons::View;
            auto dispatcher = window.Dispatcher();
            for (;;) {
                dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                api->poll_ui();
                const int current = state();
                if (current == 3 || current == 0) break;
                if (current == 4) {
                    result = saw_active ? RETURN_TO_LIBRARY : 2;
                    break;
                }
                if (current == 2 && !saw_active) {
                    saw_active = true;
                    WriteDiag("PS2: VM initialized (not a first-frame claim)");
                    // Only record the launch; the v2 API cannot distinguish paused
                    // time yet. Never add a foreign title to Eden's playtime.bin.
                    try { PlaySession history{path, 0}; } catch (...) {}
                }
                bool pressed = false;
                for (const auto& pad : Gamepad::Gamepads()) {
                    if ((pad.GetCurrentReading().Buttons & chord) == chord) { pressed = true; break; }
                }
                if (!pressed) chord_released = true;
                if (pressed && chord_released && !exit_requested) {
                    exit_requested = true;
                    api->request_exit();
                }
                ::Sleep(16);
            }
        }
    } catch (...) {
        WriteDiag("PS2: native host exception");
        if (attached) api->request_exit();
    }
    // Do not start Eden or rebuild its library swapchain while PCSX2 still
    // owns GPU/CPU resources. A failed release terminates this host session.
    if (!release()) {
        WriteDiag("PS2: release failed; refusing another engine in this process");
        return NativePs2ReleaseFailed;
    }
    return result;
}
} // namespace Jadyx
