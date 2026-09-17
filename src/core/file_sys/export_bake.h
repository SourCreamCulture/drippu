// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace FileSys {

/// One update or DLC entry that will be baked into a standalone export snapshot.
struct ExportBakeItem {
    enum class Kind {
        Update,
        Dlc,
    };

    Kind kind{Kind::Update};
    std::string name;   ///< e.g. "Update v1.2.0" or "DLC 1, 2"
    std::string source; ///< e.g. "NAND", "picked file", "game directory"
};

/// User-facing summary of what an export will bake. Header-only so the
/// exporter-smoke target can cover it without linking the emulator.
inline std::string FormatExportBakeStatus(const std::vector<ExportBakeItem>& items) {
    if (items.empty()) {
        return "Baking: base game only (no separate update or DLC). "
               "Standalone snapshot — no NAND install needed after export.";
    }

    std::string out = "Baking: ";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += "; ";
        }
        out += items[i].name;
        if (!items[i].source.empty()) {
            out += " (";
            out += items[i].source;
            out += ')';
        }
    }
    out += ". Standalone snapshot — no NAND install needed after export.";
    return out;
}

inline std::string_view ExportBakeKindName(ExportBakeItem::Kind kind) {
    switch (kind) {
    case ExportBakeItem::Kind::Update:
        return "Update";
    case ExportBakeItem::Kind::Dlc:
        return "DLC";
    }
    return "Add-on";
}

} // namespace FileSys
