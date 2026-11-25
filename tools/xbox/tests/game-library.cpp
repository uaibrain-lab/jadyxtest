// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include "jadyx/library/game_library.h"

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("eden-library-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path root; ~Cleanup() { fs::remove_all(root); } } cleanup{root};
    assert(Jadyx::ScanGameLibrary(root).entries.empty());
    assert(Jadyx::ScanGameLibrary(root / "missing").missing_root);
    fs::create_directory(root / "nested");
    for (const auto& name : {"b.XCI", "a.nsp", "ignored.txt", "partial.nsp.tmp"})
        std::ofstream{root / name};
    std::ofstream{root / "nested" / "homebrew.NRO"};
    const auto scan = Jadyx::ScanGameLibrary(root);
    assert(scan.entries.size() == 3 && scan.error.empty() && !scan.limited);
    assert(scan.entries[0].relative_path == "a.nsp");
    assert(scan.entries[1].relative_path == "b.XCI");
    assert(scan.entries[2].relative_path == fs::path{"nested"} / "homebrew.NRO");
    std::stop_source source;
    source.request_stop();
    assert(Jadyx::ScanGameLibrary(root, source.get_token()).entries.empty());
    auto deep = root;
    for (int i = 0; i < 6; ++i) { deep /= "level"; fs::create_directory(deep); }
    std::ofstream{deep / "too-deep.nsp"};
    assert(Jadyx::ScanGameLibrary(root).entries.size() == 3);
    std::error_code ec;
    fs::create_directory_symlink(root / "nested", root / "link", ec);
    if (!ec) assert(Jadyx::ScanGameLibrary(root).entries.size() == 3);
    using Jadyx::MediaKind;
    assert(Jadyx::ClassifyMediaPath("game.iso") == MediaKind::AmbiguousDisc);
    assert(Jadyx::ClassifyMediaPath("jadyx-usb:/jadyx-games-a/Disc.PS2.ISO") == MediaKind::PS2Disc);
    assert(Jadyx::ClassifyMediaPath("Disc.X360.ISO") == MediaKind::Xbox360Disc);
    assert(Jadyx::ClassifyMediaPath("folder.ps2.iso/game.iso") == MediaKind::AmbiguousDisc);
    std::ofstream{root / "disc.ps2.iso"};
    std::ofstream{root / "disc.x360.iso"};
    std::ofstream{root / "unassigned.iso"};
    const auto multi = Jadyx::ScanGameLibrary(root);
    assert(multi.entries.size() == 5);
    for (const auto& entry : multi.entries) {
        if (entry.relative_path == "disc.ps2.iso") assert(entry.platform == Jadyx::GamePlatform::PS2);
        if (entry.relative_path == "disc.x360.iso") assert(entry.platform == Jadyx::GamePlatform::Xbox360);
    }
    std::cout << "PASS library extensions, explicit ISO platform, relative paths, nesting, cancellation, empty/missing and links\n";
}
