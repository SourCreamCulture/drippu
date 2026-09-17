// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/export_bake.h"
#include "core/file_sys/export_content.h"

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

TEST_CASE("FilterAppliedBakeItems omits update unless ExeFS replace applied", "[export]") {
    const std::vector<FileSys::ExportBakeItem> candidates{
        {FileSys::ExportBakeItem::Kind::Update, "Update v1.0.0", "NAND"},
        {FileSys::ExportBakeItem::Kind::Dlc, "DLC 1", "picked file"},
    };

    const auto lied = FileSys::FilterAppliedBakeItems(candidates, false, 0);
    REQUIRE(lied.empty());

    const auto update_only = FileSys::FilterAppliedBakeItems(candidates, true, 0);
    REQUIRE(update_only.size() == 1);
    REQUIRE(update_only[0].kind == FileSys::ExportBakeItem::Kind::Update);

    const auto dlc_only = FileSys::FilterAppliedBakeItems(candidates, false, 1);
    REQUIRE(dlc_only.size() == 1);
    REQUIRE(dlc_only[0].kind == FileSys::ExportBakeItem::Kind::Dlc);

    const auto both = FileSys::FilterAppliedBakeItems(candidates, true, 2);
    REQUIRE(both.size() == 2);

    const auto dumped_without_patch_list = FileSys::FilterAppliedBakeItems({}, false, 2);
    REQUIRE(dumped_without_patch_list.size() == 1);
    REQUIRE(dumped_without_patch_list[0].kind == FileSys::ExportBakeItem::Kind::Dlc);
}

TEST_CASE("FormatFailedAddonNote fails closed on unread extras", "[export]") {
    REQUIRE(FileSys::FormatFailedAddonNote(0).empty());
    const std::string note = FileSys::FormatFailedAddonNote(2);
    REQUIRE(note.find("2 extra file") != std::string::npos);
    REQUIRE(note.find("will not continue") != std::string::npos);
}

TEST_CASE("DecideUpdateBake fails closed without Program NCA or incomplete BKTR", "[export]") {
    using FileSys::DecideUpdateBake;
    using FileSys::UpdateBakeDecision;
    using FileSys::UpdateBakeRefusal;

    REQUIRE(DecideUpdateBake(false, false, false, false) == UpdateBakeDecision::NotPresent);
    REQUIRE(UpdateBakeRefusal(UpdateBakeDecision::NotPresent) == nullptr);

    REQUIRE(DecideUpdateBake(true, false, true, false) ==
            UpdateBakeDecision::MissingBaseProgramNca);
    REQUIRE(UpdateBakeRefusal(UpdateBakeDecision::MissingBaseProgramNca) != nullptr);

    REQUIRE(DecideUpdateBake(true, true, true, false) == UpdateBakeDecision::Incomplete);
    REQUIRE(DecideUpdateBake(true, true, false, true) == UpdateBakeDecision::Incomplete);
    REQUIRE(UpdateBakeRefusal(UpdateBakeDecision::Incomplete) != nullptr);

    REQUIRE(DecideUpdateBake(true, true, true, true) == UpdateBakeDecision::Applied);
    REQUIRE(UpdateBakeRefusal(UpdateBakeDecision::Applied) == nullptr);

    REQUIRE_FALSE(FileSys::PatchHandleReplaced(true, true, true));
    REQUIRE(FileSys::PatchHandleReplaced(true, true, false));
    REQUIRE_FALSE(FileSys::PatchHandleReplaced(false, true, false));

    const auto mixed = FileSys::FilterAppliedBakeItems(
        {{FileSys::ExportBakeItem::Kind::Update, "Update v1.0.0", "NAND"}}, false, 0);
    REQUIRE(mixed.empty());
}

TEST_CASE("ExportContentSession Resolve fails closed on missing extras", "[export]") {
    Core::System system;
    system.Initialize();

    auto tmp = std::filesystem::temp_directory_path() / "drippu-export-session-test";
    std::filesystem::create_directories(tmp / "exefs");
    {
        std::ofstream out{(tmp / "exefs" / "main").string()};
        REQUIRE(out.good());
    }

    FileSys::ExportContentRequest req;
    req.rom_path = tmp.string();
    req.title_id = 0x0100AABBCCDDE000ULL;
    req.use_nand_addons = false;
    req.extra_addon_paths.push_back((tmp / "missing-update.nsp").string());

    FileSys::ExportContentSession session;
    REQUIRE_FALSE(session.Resolve(system, req));
    REQUIRE_FALSE(session.ok());
    REQUIRE_FALSE(session.GetFailedAddonPaths().empty());
    REQUIRE(session.GetError().find("will not continue") != std::string::npos);
    REQUIRE_FALSE(session.UpdateExeFSApplied());
    REQUIRE_FALSE(session.UpdateRomFSApplied());

    std::filesystem::remove_all(tmp);
}

TEST_CASE("ExportContentSession refuses empty ROM path", "[export]") {
    Core::System system;
    system.Initialize();
    FileSys::ExportContentSession session;
    FileSys::ExportContentRequest req;
    REQUIRE_FALSE(session.Resolve(system, req));
    REQUIRE(session.GetError() == "No ROM path");
}
