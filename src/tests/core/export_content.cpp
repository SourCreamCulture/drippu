// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/common_funcs.h"
#include "core/file_sys/export_bake.h"

TEST_CASE("ClassifyTitleRelation distinguishes base, update, and AOC", "[export]") {
    constexpr u64 base = 0x0100AABBCCDDE000ULL;
    constexpr u64 update = base | 0x800;
    constexpr u64 aoc0 = FileSys::GetAOCBaseTitleID(base);
    constexpr u64 aoc1 = aoc0 + 1;
    constexpr u64 other = 0x0100FFFF00000000ULL;

    REQUIRE(FileSys::ClassifyTitleRelation(base, base) == FileSys::TitleRelation::Base);
    REQUIRE(FileSys::ClassifyTitleRelation(base, update) == FileSys::TitleRelation::Update);
    REQUIRE(FileSys::ClassifyTitleRelation(base, aoc0) == FileSys::TitleRelation::Aoc);
    REQUIRE(FileSys::ClassifyTitleRelation(base, aoc1) == FileSys::TitleRelation::Aoc);
    REQUIRE(FileSys::ClassifyTitleRelation(base, other) == FileSys::TitleRelation::Unrelated);
    REQUIRE(FileSys::GetAOCID(aoc1) == 1);
}

TEST_CASE("FormatExportBakeStatus describes baked addons", "[export]") {
    const std::string empty = FileSys::FormatExportBakeStatus({});
    REQUIRE(empty.find("base game only") != std::string::npos);
    REQUIRE(empty.find("no NAND install") != std::string::npos);

    const std::vector<FileSys::ExportBakeItem> items{
        {FileSys::ExportBakeItem::Kind::Update, "Update v1.2.0", "NAND"},
        {FileSys::ExportBakeItem::Kind::Dlc, "DLC 1, 2", "picked file"},
    };
    const std::string status = FileSys::FormatExportBakeStatus(items);
    REQUIRE(status.find("Update v1.2.0") != std::string::npos);
    REQUIRE(status.find("NAND") != std::string::npos);
    REQUIRE(status.find("DLC 1, 2") != std::string::npos);
    REQUIRE(status.find("picked file") != std::string::npos);
    REQUIRE(status.find("Standalone snapshot") != std::string::npos);
}
