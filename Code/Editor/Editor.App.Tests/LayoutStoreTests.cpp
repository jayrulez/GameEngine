// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - the per-project store: the dock's layout through the file and back into a dock
// with the same panels in the same places; with none captured the dock keeps its default; the old
// dock's tree (section version 1) reads into the new layout: its splits, shares, groups and chosen
// tabs.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem> // remove_all
#include <initializer_list>

import foundation.core;
import foundation.settings;
import foundation.gui;
import foundation.gui.docking;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace
{
    constexpr StringView kDir = u8"layout-store"; // under the harness's .test-scratch

    void Register(gui::DockController& dock, const char8_t* id)
    {
        gui::DockPanelDescription description;
        description.Id = String(id);
        description.Title = String(id);
        (void)dock.Register(Move(description));
    }
    String ReadText(StringView path)
    {
        Result<Array<byte>> bytes = ReadFile(path);
        REQUIRE(bytes.HasValue());
        return String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
}

TEST_CASE("layout store: the dock's layout through the file and back; with none captured the dock keeps its default")
{
    editor::app::RegisterEditorProjectSettingsTypes();
    std::error_code ec;
    std::filesystem::remove_all(reinterpret_cast<const char*>(String(kDir).CStr()), ec);
    (void)CreateDirectories(kDir);

    gui::DockController dock(DefaultAllocator());
    Register(dock, u8"assets");
    Register(dock, u8"console");
    gui::DockLayout layout;
    REQUIRE(layout.InsertTab(u8"assets", layout.CentralStack()).Ok());
    REQUIRE(layout.InsertBeside(u8"console", layout.StackOf(u8"assets"), gui::DockSide::Bottom, 0.3f).Ok());
    REQUIRE(dock.Adopt(Move(layout)).Ok());

    foundation::settings::Settings store(DefaultAllocator());
    CHECK(editor::app::ApplyDockLayout(dock, store).Code() == ErrorCode::NotFound); // none captured: the default stands
    editor::app::CaptureDockLayout(dock, store);
    REQUIRE(editor::app::SaveProjectEditorSettings(store, kDir).IsOk());

    foundation::settings::Settings loaded(DefaultAllocator());
    REQUIRE(editor::app::LoadProjectEditorSettings(loaded, kDir).IsOk());
    gui::DockController restored(DefaultAllocator());
    Register(restored, u8"assets");
    Register(restored, u8"console");
    REQUIRE(editor::app::ApplyDockLayout(restored, loaded).IsOk());
    CHECK(restored.GetLayout().StackOf(u8"assets") != restored.GetLayout().StackOf(u8"console"));
    CHECK(restored.GetLayout().StackOf(u8"assets") == restored.GetLayout().CentralStack());
    std::filesystem::remove_all(reinterpret_cast<const char*>(String(kDir).CStr()), ec);
}

TEST_CASE("layout store: the old dock's tree (version 1) reads into the new layout, saved as the new section after")
{
    editor::app::RegisterEditorProjectSettingsTypes();
    std::error_code ec;
    std::filesystem::remove_all(reinterpret_cast<const char*>(String(kDir).CStr()), ec);
    (void)CreateDirectories(kDir);
    // The old section: a split of two tab groups, as the old dock wrote it.
    constexpr StringView kLegacy = u8R"(<root>
	<array name="sections" count="1">
		<object>
			<string name="typeNamespace">rtti::editor::editor::app</string>
			<string name="typeName">EditorDockLayoutSettings</string>
			<array name="dataVersions" count="1">
				<u64 name="type">8808931668519665070</u64>
				<u32 name="version">1</u32>
			</array>
			<object name="payload">
				<bool name="hasRoot">true</bool>
				<object name="root">
					<u32 name="type">0</u32><u32 name="direction">1</u32><f32 name="ratio">0.7</f32><i32 name="activeTab">0</i32>
					<array name="panels" count="0"/>
					<bool name="hasFirst">true</bool><bool name="hasSecond">true</bool>
					<object name="first">
						<u32 name="type">1</u32><u32 name="direction">0</u32><f32 name="ratio">0.5</f32><i32 name="activeTab">0</i32>
						<array name="panels" count="1"><string>welcome</string></array>
						<bool name="hasFirst">false</bool><bool name="hasSecond">false</bool>
					</object>
					<object name="second">
						<u32 name="type">1</u32><u32 name="direction">0</u32><f32 name="ratio">0.5</f32><i32 name="activeTab">1</i32>
						<array name="panels" count="2"><string>assets</string><string>console</string></array>
						<bool name="hasFirst">false</bool><bool name="hasSecond">false</bool>
					</object>
				</object>
			</object>
		</object>
	</array>
</root>)";
    const String path = PathJoin(kDir, editor::app::kProjectEditorSettingsFile);
    REQUIRE(WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(kLegacy.Data()), kLegacy.Size())).IsOk());

    foundation::settings::Settings loaded(DefaultAllocator());
    REQUIRE(editor::app::LoadProjectEditorSettings(loaded, kDir).IsOk()); // the store reads whole
    gui::DockController dock(DefaultAllocator());
    Register(dock, u8"welcome");
    Register(dock, u8"assets");
    Register(dock, u8"console");
    REQUIRE(editor::app::ApplyDockLayout(dock, loaded).IsOk());
    const gui::DockLayout& layout = dock.GetLayout();
    CHECK(layout.StackOf(u8"welcome") == layout.CentralStack()); // the first group, top
    const gui::DockNodeId pane = layout.StackOf(u8"assets");
    CHECK(pane != layout.CentralStack());
    CHECK(layout.StackOf(u8"console") == pane); // one group, tabbed, its second tab chosen
    REQUIRE(layout.FindNode(pane) != nullptr);
    CHECK(layout.FindNode(pane)->Active == 1u);
    // Saved again, the section is the new one.
    editor::app::CaptureDockLayout(dock, loaded);
    REQUIRE(editor::app::SaveProjectEditorSettings(loaded, kDir).IsOk());
    const String saved = ReadText(path.AsView());
    CHECK(saved.AsView().ContainsIgnoreCase(u8"<bool name=\"captured\">true</bool>"));
    std::filesystem::remove_all(reinterpret_cast<const char*>(String(kDir).CStr()), ec);
}

TEST_CASE("layout store: the old tree's nested splits each split their own region, with their shares")
{
    using editor::app::detail::LegacyDockNode;
    const auto group = [](std::initializer_list<const char8_t*> panels)
    {
        UniquePtr<LegacyDockNode> node = MakeUnique<LegacyDockNode>(DefaultAllocator());
        for (const char8_t* panel : panels)
            node->panels.PushBack(String(panel));
        return node;
    };
    const auto split = [](u32 direction, f32 ratio, UniquePtr<LegacyDockNode> first, UniquePtr<LegacyDockNode> second)
    {
        UniquePtr<LegacyDockNode> node = MakeUnique<LegacyDockNode>(DefaultAllocator());
        node->direction = direction;
        node->ratio = ratio;
        node->first = Move(first);
        node->second = Move(second);
        return node;
    };
    // Down: the documents (70%), then across: assets (65%) beside console and profiler.
    UniquePtr<LegacyDockNode> root =
        split(1, 0.7f, group({u8"welcome", u8"page-a"}), split(0, 0.65f, group({u8"assets"}), group({u8"console", u8"profiler"})));
    gui::DockLayout layout = editor::app::LayoutFromLegacy(*root);

    const gui::DockNodeId documents = layout.CentralStack();
    CHECK(layout.StackOf(u8"welcome") == documents);
    CHECK(layout.StackOf(u8"page-a") == documents);
    const gui::DockNodeId assets = layout.StackOf(u8"assets");
    const gui::DockNodeId console = layout.StackOf(u8"console");
    CHECK(assets != documents);
    CHECK(console != assets);
    CHECK(layout.StackOf(u8"profiler") == console);

    // The documents over the pane, the pane's two side by side: each stack's parent split.
    const auto parentOf = [&layout](gui::DockNodeId id) -> const gui::DockNode*
    {
        for (const gui::DockNode& node : layout.Nodes())
            for (gui::DockNodeId child : node.Children)
                if (child == id)
                    return &node;
        return nullptr;
    };
    const gui::DockNode* across = parentOf(assets);
    REQUIRE(across != nullptr);
    CHECK(parentOf(console) == across);
    CHECK(across->Axis == gui::DockAxis::Horizontal);
    REQUIRE(across->Weights.Size() == 2u);
    CHECK(across->Weights[0] / (across->Weights[0] + across->Weights[1]) == doctest::Approx(0.65f).epsilon(0.01));
    const gui::DockNode* down = parentOf(documents);
    REQUIRE(down != nullptr);
    CHECK(down->Axis == gui::DockAxis::Vertical);
    REQUIRE(down->Weights.Size() == 2u);
    CHECK(down->Weights[0] / (down->Weights[0] + down->Weights[1]) == doctest::Approx(0.7f).epsilon(0.01));
}

TEST_CASE("layout store: what a restored layout names and nothing provides (a Game tab) is let go, docked or closed")
{
    gui::DockController dock(DefaultAllocator());
    Register(dock, u8"assets");
    Register(dock, u8"game-page");
    Register(dock, u8"game-page-1");
    gui::DockLayout layout;
    REQUIRE(layout.InsertTab(u8"assets", layout.CentralStack()).Ok());
    REQUIRE(layout.InsertBeside(u8"game-page", layout.StackOf(u8"assets"), gui::DockSide::End, 0.5f).Ok());
    REQUIRE(layout.InsertTab(u8"game-page-1", layout.StackOf(u8"game-page")).Ok());
    REQUIRE(layout.Close(u8"game-page-1").Ok()); // a closed one is remembered as hidden
    const gui::DockLayout saved = layout;

    // The next session: the shared panels registered, no Game tab reopened.
    gui::DockController restored(DefaultAllocator());
    Register(restored, u8"assets");
    REQUIRE(restored.Adopt(saved).Ok());
    CHECK(restored.GetLayout().StackOf(u8"game-page") != gui::kNoDock); // a placeholder until let go
    editor::app::ForgetUnprovidedPanels(restored);
    const gui::DockLayout& kept = restored.GetLayout();
    CHECK(kept.StackOf(u8"assets") != gui::kNoDock);
    CHECK(kept.StackOf(u8"game-page") == gui::kNoDock);
    CHECK(kept.FindHidden(u8"game-page") == nullptr);
    CHECK(kept.FindHidden(u8"game-page-1") == nullptr);
    CHECK(kept.CheckInvariants());
}
