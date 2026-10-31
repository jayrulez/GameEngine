// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :layout partition.
//
// Per-project editor state, kept in the structured settings store (foundation.settings): one
// file, <project>/Editor/editor.project.settings.xml, holding typed, versioned sections:
//
//   EditorDockLayoutSettings: the dock's layout (GUI.Docking's DockLayoutDocument), panels by
//                             their ids; a panel the layout names that is not registered keeps
//                             its place until it is. Version 2; version 1 held an older binary
//                             dock tree, which still reads into the new layout (its splits,
//                             shares, tab groups and chosen tabs).
//   EditorFavoritesSettings:  pinned asset guids (EditorContext favorites)
//   EditorOpenPagesSettings:  open page instance guids and the active one
//
// Other modules add their own sections to the same store (e.g. the material page's preview
// prefs) through EditorContext::ProjectEditorSettings(); their types must be registered
// before the app loads the store (registerEditors runs first, so per-subsystem registration
// belongs in each RegisterXxxEditor).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

export module editor.app:layout;

import foundation.core;
import foundation.vfs;
import foundation.settings;
import foundation.xml.serialization;
import editor.core; // EditorContext (favorites)
import foundation.gui.docking; // DockLayoutDocument, DockController

using namespace foundation::core;
namespace vfs = foundation::vfs;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace settings = foundation::settings;

    // The one per-project editor-state file (hand-editable XML, like every settings store).
    inline constexpr StringView kProjectEditorSettingsFile = u8"editor.project.settings.xml";

    namespace detail
    {
        // Version 1's dock tree: a node is either a split of two children, with the first's share
        // `ratio` (left or top), across (`direction` 0) or down (1), or a tab group of panels.
        struct LegacyDockNode
        {
            u32 direction = 0;
            f32 ratio = 0.5f;
            i32 activeTab = 0;
            Array<String> panels;
            UniquePtr<LegacyDockNode> first;
            UniquePtr<LegacyDockNode> second;
            [[nodiscard]] bool IsSplit() const noexcept { return first && second; }
        };

        inline void ReadLegacyDockNode(ISerializer& ar, LegacyDockNode& node)
        {
            ar.BeginObject();
            u32 type = 0; // the node's kind is inferred from its children, not from this
            foundation::core::Serialize(ar, "type", type);
            foundation::core::Serialize(ar, "direction", node.direction);
            foundation::core::Serialize(ar, "ratio", node.ratio);
            foundation::core::Serialize(ar, "activeTab", node.activeTab);
            foundation::core::Serialize(ar, "panels", node.panels);
            bool hasFirst = false;
            bool hasSecond = false;
            foundation::core::Serialize(ar, "hasFirst", hasFirst);
            foundation::core::Serialize(ar, "hasSecond", hasSecond);
            if (hasFirst)
            {
                node.first = MakeUnique<LegacyDockNode>(editor::EditorRootAllocator());
                ar.Key("first");
                ReadLegacyDockNode(ar, *node.first);
            }
            if (hasSecond)
            {
                node.second = MakeUnique<LegacyDockNode>(editor::EditorRootAllocator());
                ar.Key("second");
                ReadLegacyDockNode(ar, *node.second);
            }
            ar.EndObject();
        }

        // The first panel of a subtree, in order (left or top first); null when it holds none.
        [[nodiscard]] inline const String* FirstLegacyPanel(const LegacyDockNode& node)
        {
            if (node.IsSplit())
            {
                if (const String* found = FirstLegacyPanel(*node.first))
                    return found;
                return FirstLegacyPanel(*node.second);
            }
            return node.panels.IsEmpty() ? nullptr : &node.panels[0];
        }

        // Places `node`'s panels into `stack` and stacks split from it. `stack` holds nothing
        // yet, or the node's first panel: a split puts its second half's first panel beside it
        // (taking the second half's share), so each half then splits only its own stack.
        inline void PlaceLegacyNode(gui::DockLayout& layout, const LegacyDockNode& node, gui::DockNodeId stack)
        {
            if (!node.IsSplit())
            {
                for (const String& panel : node.panels)
                    if (layout.StackOf(panel.AsView()) == gui::kNoDock)
                        (void)layout.InsertTab(panel.AsView(), stack);
                if (node.activeTab >= 0 && static_cast<usize>(node.activeTab) < node.panels.Size())
                    (void)layout.Activate(node.panels[static_cast<usize>(node.activeTab)].AsView());
                return;
            }
            const String* firstPanel = FirstLegacyPanel(*node.first);
            const String* secondPanel = FirstLegacyPanel(*node.second);
            if (firstPanel == nullptr || secondPanel == nullptr)
            {
                PlaceLegacyNode(layout, firstPanel != nullptr ? *node.first : *node.second, stack);
                return;
            }
            if (layout.StackOf(firstPanel->AsView()) == gui::kNoDock)
                (void)layout.InsertTab(firstPanel->AsView(), stack);
            const gui::DockSide side = node.direction == 0 ? gui::DockSide::End : gui::DockSide::Bottom;
            const f32 share = Clamp(1.0f - node.ratio, 0.05f, 0.95f);
            if (!layout.InsertBeside(secondPanel->AsView(), stack, side, share).Ok())
            {
                PlaceLegacyNode(layout, *node.first, stack); // a panel named twice: place the first half alone
                return;
            }
            const gui::DockNodeId beside = layout.StackOf(secondPanel->AsView());
            PlaceLegacyNode(layout, *node.first, stack);
            PlaceLegacyNode(layout, *node.second, beside);
        }
    }

    /// Converts a version 1 dock tree to the GUI dock's layout: its first panel's group in the central stack,
    /// each split's halves beside one another with the same shares, each group's tabs in order with
    /// its chosen one. A panel named twice keeps its first place.
    [[nodiscard]] inline gui::DockLayout LayoutFromLegacy(const detail::LegacyDockNode& root)
    {
        gui::DockLayout layout;
        detail::PlaceLegacyNode(layout, root, layout.CentralStack());
        return layout;
    }

    // The dock's layout: its document when one was captured.
    class EditorDockLayoutSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorDockLayoutSettings, ISerializable)
    public:
        bool captured = false;
        gui::DockLayoutDocument document;

        void Serialize(ISerializer& ar) override
        {
            if (ar.Mode() == SerializeMode::Read && ar.Version() == 1)
            {
                // A version 1 dock tree, converted into the GUI dock's layout (saved as version 2
                // afterwards).
                bool hasRoot = false;
                foundation::core::Serialize(ar, "hasRoot", hasRoot);
                captured = false;
                if (hasRoot)
                {
                    detail::LegacyDockNode root;
                    ar.Key("root");
                    detail::ReadLegacyDockNode(ar, root);
                    document.Layout = LayoutFromLegacy(root);
                    captured = detail::FirstLegacyPanel(root) != nullptr;
                }
                return;
            }
            foundation::core::Serialize(ar, "captured", captured);
            if (captured)
            {
                ar.Key("layout");
                ar.BeginObject();
                document.Serialize(ar);
                ar.EndObject();
            }
        }
    };

    class EditorFavoritesSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorFavoritesSettings, ISerializable)
    public:
        Array<Guid> favorites;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "favorites", favorites);
        }
    };

    class EditorOpenPagesSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorOpenPagesSettings, ISerializable)
    public:
        Array<Guid> pages;
        Guid active;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "pages", pages);
            ar.Key("active");
            ar.GuidValue(active);
        }
    };

    // The asset browser's view mode (list or grid), persisted so a project reopens in the last-used
    // view. A view toggle is per-project UI state, not a project setting, so it lives here with the
    // layout and open pages rather than in the project's data.
    class EditorAssetBrowserSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorAssetBrowserSettings, ISerializable)
    public:
        bool gridMode = false;       // false = list, true = grid
        bool pickerGridMode = false; // the asset picker's own list or grid choice (AssetPickerWindow)

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "gridMode", gridMode);
            foundation::core::SerializeAppended(ar, "pickerGridMode", pickerGridMode);
        }
    };

    // Register the app-side section types (call once at startup, before any store Load).
    inline void RegisterEditorProjectSettingsTypes()
    {
        GlobalTypeRegistry().Register(EditorDockLayoutSettings::StaticType(),
                                      TypeDomain(u8"Editor"));
        RegisterSerializable<EditorDockLayoutSettings>();
        // Version 1 (the older dock tree) still reads into the new layout through a legacy reader
        // one version back, to be removed once projects have saved past it.
        KeepLegacyReader(const_cast<TypeInfo&>(EditorDockLayoutSettings::StaticType()), 1);
        GlobalTypeRegistry().Register(EditorFavoritesSettings::StaticType(),
                                      TypeDomain(u8"Editor"));
        RegisterSerializable<EditorFavoritesSettings>();
        GlobalTypeRegistry().Register(EditorOpenPagesSettings::StaticType(),
                                      TypeDomain(u8"Editor"));
        RegisterSerializable<EditorOpenPagesSettings>();
        GlobalTypeRegistry().Register(EditorAssetBrowserSettings::StaticType(),
                                      TypeDomain(u8"Editor"));
        RegisterSerializable<EditorAssetBrowserSettings>();
    }

    // ---- store <-> live state ------------------------------------------------------------

    // Captures the dock's layout into the store.
    inline void CaptureDockLayout(const gui::DockController& dock, settings::Settings& store)
    {
        EditorDockLayoutSettings& section = store.Section<EditorDockLayoutSettings>();
        section.document.Layout = dock.GetLayout();
        section.captured = true;
        store.MarkChanged<EditorDockLayoutSettings>();
    }

    // Applies the store's layout to `dock` (its registered panels placed where it says, the rest at
    // their defaults). NotFound when none was captured (the caller keeps its default layout); a
    // refusal (a layout that breaks the dock's rules) is logged and leaves the dock as it was.
    [[nodiscard]] inline Status ApplyDockLayout(gui::DockController& dock, settings::Settings& store)
    {
        const EditorDockLayoutSettings* section = store.Find<EditorDockLayoutSettings>();
        if (section == nullptr || !section->captured)
            return Status{ErrorCode::NotFound};
        const gui::DockOutcome outcome = dock.Adopt(section->document.Layout);
        if (!outcome.Ok())
        {
            LOG_WARNING(u8"Editor", u8"the saved dock layout was refused ({}); the default stands", outcome.Refused.AsView());
            return Status{ErrorCode::InvalidArgument};
        }
        return Status{};
    }

    // Forgets the panels a restored layout names that nothing provides once the project's pages
    // have reopened (a Game tab, which no project reopens; a page whose asset is gone), whether
    // docked or closed, so the dock shows no placeholder for them.
    inline void ForgetUnprovidedPanels(gui::DockController& dock)
    {
        gui::DockLayout layout = dock.GetLayout();
        Array<String> gone;
        for (const gui::DockNode& node : layout.Nodes())
            if (node.IsStack())
                for (const String& panel : node.Panels)
                    if (dock.Describe(panel.AsView()) == nullptr)
                        gone.PushBack(panel);
        for (const String& panel : gone)
            (void)layout.Close(panel.AsView());
        for (const gui::DockHidden& hidden : layout.Hidden())
            if (dock.Describe(hidden.Panel.AsView()) == nullptr)
                gone.PushBack(hidden.Panel);
        if (gone.IsEmpty())
            return;
        for (const String& panel : gone)
            (void)layout.Forget(panel.AsView());
        (void)dock.Adopt(Move(layout));
    }

    inline void CaptureFavorites(editor::EditorContext& context,
                                 settings::Settings& store)
    {
        EditorFavoritesSettings& section = store.Section<EditorFavoritesSettings>();
        section.favorites.Clear();
        for (const Guid& id : context.Favorites())
        {
            section.favorites.PushBack(id);
        }
        store.MarkChanged<EditorFavoritesSettings>();
    }

    inline void ApplyFavorites(editor::EditorContext& context,
                               settings::Settings& store)
    {
        if (const EditorFavoritesSettings* section = store.Find<EditorFavoritesSettings>())
        {
            Array<Guid> favorites;
            for (const Guid& id : section->favorites)
            {
                favorites.PushBack(id);
            }
            context.SetFavorites(Move(favorites));
        }
    }

    inline void CaptureOpenPages(settings::Settings& store, const Array<Guid>& pages,
                                 const Guid& activePage)
    {
        EditorOpenPagesSettings& section = store.Section<EditorOpenPagesSettings>();
        section.pages = pages;
        section.active = activePage;
        store.MarkChanged<EditorOpenPagesSettings>();
    }

    // NotFound when no page set was ever saved (first launch: the caller opens the default scene).
    [[nodiscard]] inline Status ApplyOpenPages(settings::Settings& store, Array<Guid>& outPages,
                                               Guid& outActivePage)
    {
        const EditorOpenPagesSettings* section = store.Find<EditorOpenPagesSettings>();
        if (section == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        outPages = section->pages;
        outActivePage = section->active;
        return Status{};
    }

    // ---- file I/O ------------------------------------------------------------------------

    // Loads the per-project store from <directory>/editor.project.settings.xml. NotFound when
    // absent (a fresh project, or a first run): the store stays empty and every section reads
    // as defaults. Section types must be registered first.
    [[nodiscard]] inline Status LoadProjectEditorSettings(settings::Settings& store,
                                                          StringView directory)
    {
        vfs::NativeFileSystem root(directory, editor::EditorRootAllocator());
        UniquePtr<IStream> stream = root.Open(kProjectEditorSettingsFile, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        return store.Load(*stream, foundation::xml::XmlSerializerFactory());
    }

    // Persists the per-project store; on success, deletes the older per-file state (layout.xml,
    // favorites.bin, pages.bin) so stale state cannot shadow the store.
    [[nodiscard]] inline Status SaveProjectEditorSettings(const settings::Settings& store,
                                                          StringView directory)
    {
        MemoryStream buffer;
        if (Status s = store.Save(buffer, foundation::xml::XmlSerializerFactory()); !s.IsOk())
        {
            return s;
        }
        vfs::NativeFileSystem root(directory, editor::EditorRootAllocator());
        vfs::IWritableFileSystem* writable = root.AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }
        const Status saved = writable->Save(kProjectEditorSettingsFile, buffer.Bytes());
        if (saved.IsOk())
        {
            (void)FileDelete(PathJoin(directory, u8"layout.xml"));
            (void)FileDelete(PathJoin(directory, u8"favorites.bin"));
            (void)FileDelete(PathJoin(directory, u8"pages.bin"));
        }
        return saved;
    }

    RTTI_DEFINE_OBJECT_VERSIONED(EditorDockLayoutSettings, "rtti::editor::editor::app", 2)
    RTTI_DEFINE_OBJECT_VERSIONED(EditorFavoritesSettings, "rtti::editor::editor::app", 1)
    RTTI_DEFINE_OBJECT_VERSIONED(EditorOpenPagesSettings, "rtti::editor::editor::app", 1)
    RTTI_DEFINE_OBJECT_VERSIONED(EditorAssetBrowserSettings, "rtti::editor::editor::app", 1)
}
