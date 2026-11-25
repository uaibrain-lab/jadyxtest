// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <windows.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Gaming.Input.h>
#include "jadyx/app/boot_session.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/play_history.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/uwp_rom_storage.h"

namespace Jadyx {
inline constexpr int NativeXeniaUnavailable = 10001;
inline constexpr int NativeXeniaReleaseFailed = 10003;

inline bool NativeXeniaCandidate(std::string_view path) {
    const auto kind = ClassifyMediaPath(path);
    return kind == MediaKind::Xbox360Executable || kind == MediaKind::Xbox360Disc;
}

inline bool NativeXeniaStaged() {
    try {
        const std::filesystem::path root{
            winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()};
        return std::filesystem::is_regular_file(root / L"Xenia-Native-Dependencies.json") &&
               std::filesystem::is_regular_file(root / L"uaibrain_xenia_embedded.dll");
    } catch (...) { return false; }
}

// Owns a single foreign guest on the existing UI thread. The caller has
// already released the library and splash presenters; it may only resume
// them after release returns success. No DLL is loaded from writable storage.
inline int RunNativeXenia(const winrt::Windows::UI::Core::CoreWindow& window,
                          const BootSurface& surface, const std::string& path) {
    if (!NativeXeniaStaged() || !NativeXeniaCandidate(path)) return NativeXeniaUnavailable;
    using namespace winrt::Windows::UI::Core;
    using namespace winrt::Windows::Gaming::Input;
    bool closed = false;
    bool focused = true;
    std::wstring native_game;
    try {
        auto close = window.Closed(winrt::auto_revoke, [&](auto&&, auto&&) { closed = true; });
        if (IsStoragePath(path)) {
            auto task = std::async(std::launch::async, [path] { return ResolveDirectExternalGame(path); });
            while (task.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                ::Sleep(16);
            }
            native_game = task.get();
        } else {
            native_game = winrt::to_hstring(path).c_str();
        }
        if (closed) return 0;
        if (native_game.empty() || !std::filesystem::path(native_game).is_absolute()) return 16;
    } catch (...) { WriteDiag("Xenia: media resolution failed"); return 16; }

    static HMODULE module = LoadPackagedLibrary(L"uaibrain_xenia_embedded.dll", 0);
    if (!module) { WriteDiag("Xenia: packaged DLL unavailable"); return NativeXeniaUnavailable; }
    using Version = uint32_t (*)();
    using Create = void* (*)(IUnknown*, uint32_t, uint32_t, const wchar_t*);
    using Boot = int (*)(void*, const wchar_t*);
    using Tick = int (*)(void*, uint32_t, uint32_t, int);
    using Release = int (*)(void*);
    const auto version = reinterpret_cast<Version>(GetProcAddress(module, "uaibrain_xenia_core_abi_v1"));
    const auto create = reinterpret_cast<Create>(GetProcAddress(module, "uaibrain_xenia_core_create_v1"));
    const auto boot = reinterpret_cast<Boot>(GetProcAddress(module, "uaibrain_xenia_core_boot_v1"));
    const auto tick = reinterpret_cast<Tick>(GetProcAddress(module, "uaibrain_xenia_core_tick_v1"));
    const auto release = reinterpret_cast<Release>(GetProcAddress(module, "uaibrain_xenia_core_release_v1"));
    if (!version || !create || !boot || !tick || !release || version() != 1)
        return NativeXeniaUnavailable;

    void* handle = nullptr;
    int result = 2;
    try {
        auto close = window.Closed(winrt::auto_revoke, [&](auto&&, auto&&) { closed = true; });
        auto activation = window.Activated(winrt::auto_revoke, [&](auto&&, const WindowActivatedEventArgs& args) {
            focused = args.WindowActivationState() != CoreWindowActivationState::Deactivated;
        });
        const std::filesystem::path local{
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()};
        const auto root = local / L"xenia";
        handle = create(static_cast<IUnknown*>(surface.core_window), surface.width, surface.height, root.c_str());
        if (handle && boot(handle, native_game.c_str()) == 1) {
            bool saw_running = false;
            bool chord_released = false;
            const auto chord = GamepadButtons::Menu | GamepadButtons::View;
            auto dispatcher = window.Dispatcher();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            for (;;) {
                dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                if (closed) { result = 0; break; }
                const int state = tick(handle, surface.width, surface.height, focused ? 1 : 0);
                if (state == -1 || state < -1 || state > 2) break;
                if (state == 2) { result = saw_running ? RETURN_TO_LIBRARY : 2; break; }
                if (state == 1 && !saw_running) {
                    saw_running = true;
                    WriteDiag("Xenia: guest running (not first-frame proof)");
                    try { PlaySession history{path, 0}; } catch (...) {}
                }
                bool pressed = false;
                for (const auto& pad : Gamepad::Gamepads()) {
                    if ((pad.GetCurrentReading().Buttons & chord) == chord) { pressed = true; break; }
                }
                if (!pressed) chord_released = true;
                if (focused && pressed && chord_released) { result = RETURN_TO_LIBRARY; break; }
                if (!saw_running && std::chrono::steady_clock::now() >= deadline) break;
                ::Sleep(16);
            }
        }
    } catch (...) { WriteDiag("Xenia: host session exception"); }
    // A failed release still owns the CoreWindow. Do not restore Eden.
    if (handle && !release(handle)) {
        WriteDiag("Xenia: release failed; refusing to restore host presenter");
        return NativeXeniaReleaseFailed;
    }
    return closed ? 0 : result;
}
} // namespace Jadyx
