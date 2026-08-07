// EditorShell + dock-layout persistence tests (headless: views build and lay out without a
// window, same as the toolkit's DockPersistenceTests). Covers: chrome construction (bars, dock,
// the five persistence-id'd panels), status routing, and the save -> restore -> same-layout
// round-trip through the XML file in a project's Editor/ directory.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.xml.serialization;
import draconic.settings;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;
using namespace draconic::editor;
using namespace draconic::editor::app;

namespace ui = draconic::ui;

namespace
{
    void RemoveStateDir(StringView dir)
    {
        FileDelete(PathJoin(dir, u8"layout.xml"));
        RemoveDirectory(dir);
    }
}

TEST_CASE("editor-shell: builds the chrome with the global panels only")
{
    EditorContext ctx;
    EditorShell shell;
    shell.Build(ctx, nullptr, 1280, 720);

    REQUIRE(shell.Root() != nullptr);
    REQUIRE(shell.Menus() != nullptr);
    REQUIRE(shell.StatusBar() != nullptr);
    REQUIRE(shell.Docks() != nullptr);

    // Global panels are Assets + Console (+ the Welcome center placeholder) - scene-scoped
    // views (viewport/hierarchy/inspector) live INSIDE pages, never in the shell (multi-scene).
    REQUIRE(shell.WelcomePanel() != nullptr);
    CHECK(shell.WelcomePanel()->PersistenceId() == u8"welcome");
    CHECK(shell.ConsolePanel()->PersistenceId() == u8"console");
    CHECK(shell.AssetsPanel()->PersistenceId() == u8"assets");
    CHECK(shell.Docks()->FindPanelById(u8"assets") == shell.AssetsPanel());
    CHECK(shell.Docks()->FindPanelById(u8"viewport") == nullptr); // no global viewport panel

    // Status text routes through the context to the status bar.
    ctx.SetStatus(u8"hello"); // must not crash; the label text lives inside the bar
}

TEST_CASE("editor-shell: page panels dock into the center document area as closable tabs")
{
    EditorContext ctx;
    EditorShell shell;
    shell.Build(ctx, nullptr, 1280, 720);

    auto content = MakeRef<draconic::ui::Label>(DefaultAllocator(), StringView(u8"scene content"));
    ui::toolkit::DockablePanel* page = shell.AddPagePanel(u8"Scene 1", content.Get());
    REQUIRE(page != nullptr);

    // The page tabs with the Welcome panel in the center group (same parent tab group).
    CHECK(page->Parent == shell.WelcomePanel()->Parent);
}

TEST_CASE("editor-shell: dock layout survives a save/restore round-trip")
{
    const StringView dir = u8"draconic_editor_test_layout";
    RemoveStateDir(dir);
    REQUIRE(CreateDirectory(dir));

    EditorContext ctx;
    EditorShell shell;
    shell.Build(ctx, nullptr, 1280, 720);

    // Capture the default arrangement into the per-project settings store, persist it, and
    // load it back through the same file the app writes (the unified store).
    RegisterEditorProjectSettingsTypes();
    UniquePtr<ui::toolkit::DockLayoutNode> before = shell.Docks()->ExportLayout();
    REQUIRE(static_cast<bool>(before));
    draconic::settings::Settings store;
    REQUIRE(shell.SaveLayout(store).IsOk());
    REQUIRE(SaveProjectEditorSettings(store, dir).IsOk());
    CHECK(FileExists(PathJoin(dir, kProjectEditorSettingsFile)));

    // Rearrange (undock a panel), then restore from a FRESHLY LOADED store: the exported
    // tree matches the saved one again.
    shell.Docks()->UndockPanel(shell.AssetsPanel());
    CHECK(shell.Docks()->FindPanelById(u8"assets") != nullptr); // still registered while undocked
    draconic::settings::Settings loaded;
    REQUIRE(LoadProjectEditorSettings(loaded, dir).IsOk());
    REQUIRE(shell.RestoreLayout(loaded).IsOk());
    UniquePtr<ui::toolkit::DockLayoutNode> after = shell.Docks()->ExportLayout();
    REQUIRE(static_cast<bool>(after));

    // Structural comparison: same node types, same panel ids in the same tab order.
    struct Compare
    {
        static bool Nodes(const ui::toolkit::DockLayoutNode* a,
                          const ui::toolkit::DockLayoutNode* b)
        {
            if ((a == nullptr) != (b == nullptr))
            {
                return false;
            }
            if (a == nullptr)
            {
                return true;
            }
            if (a->Type != b->Type)
            {
                return false;
            }
            if (a->Type == ui::toolkit::DockLayoutNodeType::TabGroup)
            {
                if (a->PanelIds.Size() != b->PanelIds.Size())
                {
                    return false;
                }
                for (usize i = 0; i < a->PanelIds.Size(); ++i)
                {
                    if (a->PanelIds[i] != b->PanelIds[i])
                    {
                        return false;
                    }
                }
                return true;
            }
            return a->Direction == b->Direction && Nodes(a->First.Get(), b->First.Get()) &&
                   Nodes(a->Second.Get(), b->Second.Get());
        }
    };
    CHECK(Compare::Nodes(before.Get(), after.Get()));

    RemoveStateDir(dir);
}

TEST_CASE("editor-layout: restore from a missing file reports NotFound")
{
    const StringView dir = u8"draconic_editor_test_layout_missing";
    RemoveStateDir(dir);
    REQUIRE(CreateDirectory(dir));

    EditorContext ctx;
    EditorShell shell;
    shell.Build(ctx, nullptr, 640, 480);
    // A store with no captured snapshot (and a directory with no store file) both = NotFound.
    draconic::settings::Settings store;
    CHECK(shell.RestoreLayout(store).Code() == ErrorCode::NotFound);
    CHECK(LoadProjectEditorSettings(store, dir).Code() == ErrorCode::NotFound);

    RemoveStateDir(dir);
}

TEST_CASE("editor-layout: layout node round-trips nested splits through XML")
{
    // A hand-built split tree: [A | (B tabbed C)] over D - exercises nesting, ratios, tab order.
    ui::toolkit::DockLayoutNode root;
    root.Type = ui::toolkit::DockLayoutNodeType::Split;
    root.Direction = draconic::ui::Orientation::Vertical;
    root.SplitRatio = 0.75f;
    root.First = MakeUnique<ui::toolkit::DockLayoutNode>(DefaultAllocator());
    root.First->Type = ui::toolkit::DockLayoutNodeType::Split;
    root.First->Direction = draconic::ui::Orientation::Horizontal;
    root.First->SplitRatio = 0.25f;
    root.First->First = MakeUnique<ui::toolkit::DockLayoutNode>(DefaultAllocator());
    root.First->First->PanelIds.PushBack(String(u8"a"));
    root.First->Second = MakeUnique<ui::toolkit::DockLayoutNode>(DefaultAllocator());
    root.First->Second->PanelIds.PushBack(String(u8"b"));
    root.First->Second->PanelIds.PushBack(String(u8"c"));
    root.First->Second->ActiveTabIndex = 1;
    root.Second = MakeUnique<ui::toolkit::DockLayoutNode>(DefaultAllocator());
    root.Second->PanelIds.PushBack(String(u8"d"));

    // Write to XML text, read back.
    MemoryStream buffer;
    {
        SerializerFactory factory = draconic::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Write);
        REQUIRE(ctx->serializer != nullptr);
        SerializeLayoutNode(*ctx->serializer, root);
        REQUIRE(ctx->serializer->IsOk());
        ctx->Flush(buffer);
    }

    ui::toolkit::DockLayoutNode loaded;
    {
        (void)buffer.Seek(0, SeekOrigin::Begin); // reuse the write stream for reading
        SerializerFactory factory = draconic::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Read);
        REQUIRE(ctx->serializer != nullptr);
        SerializeLayoutNode(*ctx->serializer, loaded);
        REQUIRE(ctx->serializer->IsOk());
    }

    CHECK(loaded.Type == ui::toolkit::DockLayoutNodeType::Split);
    CHECK(loaded.Direction == draconic::ui::Orientation::Vertical);
    CHECK(loaded.SplitRatio == doctest::Approx(0.75f));
    REQUIRE(static_cast<bool>(loaded.First));
    REQUIRE(static_cast<bool>(loaded.Second));
    CHECK(loaded.First->SplitRatio == doctest::Approx(0.25f));
    REQUIRE(static_cast<bool>(loaded.First->Second));
    REQUIRE(loaded.First->Second->PanelIds.Size() == 2);
    CHECK(loaded.First->Second->PanelIds[0] == u8"b");
    CHECK(loaded.First->Second->PanelIds[1] == u8"c");
    CHECK(loaded.First->Second->ActiveTabIndex == 1);
    REQUIRE(loaded.Second->PanelIds.Size() == 1);
    CHECK(loaded.Second->PanelIds[0] == u8"d");
}
