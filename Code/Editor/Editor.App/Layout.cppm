// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :layout partition.
//
// PER-PROJECT editor-state persistence, UNIFIED on the structured settings store
// (foundation.settings): ONE file - <project>/Editor/editor.project.settings.xml - holding
// typed, versioned sections instead of the former bespoke trio (layout.xml + favorites.bin +
// pages.bin; those are deleted on the first save, no legacy read):
//
//   EditorDockLayoutSettings  - the dock's layout (GUI.Docking's DockLayoutDocument), panels
//                               by their ids; a panel the layout names that is not registered
//                               keeps its place until it is (version 2; version 1 held the old
//                               dock's binary tree, read once and set aside: the dock then starts
//                               from its default, the open pages still reopening)
//   EditorFavoritesSettings   - pinned asset guids (EditorContext favorites)
//   EditorOpenPagesSettings   - open page instance guids + the active one
//
// Other modules add their own sections to the SAME store (e.g. the material page's preview
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
        // Version 1's dock tree (the old dock's), read only to be set aside: a node is a tab
        // group (its panels) or a split (its two children), each written with presence flags.
        inline void SkipLegacyLayoutNode(ISerializer& ar)
        {
            ar.BeginObject();
            u32 type = 0;
            u32 direction = 0;
            f32 ratio = 0.0f;
            i32 activeTab = 0;
            Array<String> panels;
            foundation::core::Serialize(ar, "type", type);
            foundation::core::Serialize(ar, "direction", direction);
            foundation::core::Serialize(ar, "ratio", ratio);
            foundation::core::Serialize(ar, "activeTab", activeTab);
            foundation::core::Serialize(ar, "panels", panels);
            bool hasFirst = false;
            bool hasSecond = false;
            foundation::core::Serialize(ar, "hasFirst", hasFirst);
            foundation::core::Serialize(ar, "hasSecond", hasSecond);
            if (hasFirst)
            {
                ar.Key("first");
                SkipLegacyLayoutNode(ar);
            }
            if (hasSecond)
            {
                ar.Key("second");
                SkipLegacyLayoutNode(ar);
            }
            ar.EndObject();
        }
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
                // The old dock's tree: read past and dropped (a layout from it would not map onto
                // the GUI's dock reliably); the dock starts from its default.
                bool hasRoot = false;
                foundation::core::Serialize(ar, "hasRoot", hasRoot);
                if (hasRoot)
                {
                    ar.Key("root");
                    detail::SkipLegacyLayoutNode(ar);
                }
                captured = false;
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

    // The asset browser's view mode (list vs grid): persisted so a project reopens in the last-used
    // view. Per-project, alongside the other editor-state sections (a view toggle is UI state, not a
    // project setting - it lives here with layout / open pages, not in the project's data).
    class EditorAssetBrowserSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorAssetBrowserSettings, ISerializable)
    public:
        bool gridMode = false;       // false = list, true = grid
        bool pickerGridMode = false; // the asset picker dialog's own list / grid choice

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "gridMode", gridMode);
            foundation::core::SerializeAppended(ar, "pickerGridMode", pickerGridMode); // added 2026-10-10
        }
    };

    // Register the app-side section types (call once at startup, before any store Load).
    inline void RegisterEditorProjectSettingsTypes()
    {
        GlobalTypeRegistry().Register(EditorDockLayoutSettings::StaticType(),
                                      TypeDomain(u8"Editor"));
        RegisterSerializable<EditorDockLayoutSettings>();
        // Version 1 (the old dock's tree) still reads, to be set aside: a legacy reader one
        // version back, removed once projects have saved past it (Process/CONVENTIONS.md).
        const_cast<TypeInfo&>(EditorDockLayoutSettings::StaticType()).minReadDataVersion = 1;
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

    // The dock's layout into the store.
    inline void CaptureDockLayout(const gui::DockController& dock, settings::Settings& store)
    {
        EditorDockLayoutSettings& section = store.Section<EditorDockLayoutSettings>();
        section.document.Layout = dock.GetLayout();
        section.captured = true;
        store.MarkChanged<EditorDockLayoutSettings>();
    }

    // The store's layout into `dock` (its registered panels placed where it says, the rest at
    // their defaults). NotFound when none was captured (the caller keeps its default layout);
    // a refusal (a layout that breaks the dock's rules) said in the log, the dock as it was.
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

    // NotFound when no page set was ever saved (first launch: caller opens the default doc).
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

    // Load the per-project store from <directory>/editor.project.settings.xml. NotFound when
    // absent (fresh project / first run with the unified store) - the store stays empty and
    // every section reads as defaults. Section types must be registered first.
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

    // Persist the per-project store; on success, delete the pre-unification bespoke files
    // (layout.xml / favorites.bin / pages.bin) so stale state can't shadow the store.
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
