// Draconic::EditorApp - :layout partition.
//
// PER-PROJECT editor-state persistence, UNIFIED on the structured settings store
// (draconic.settings): ONE file - <project>/Editor/editor.project.settings.xml - holding
// typed, versioned sections instead of the former bespoke trio (layout.xml + favorites.bin +
// pages.bin; those are deleted on the first save, no legacy read):
//
//   EditorDockLayoutSettings  - the dock tree (DockManager Export/ApplyLayout snapshot);
//                               panels matched back by PersistenceId, unknown ids skipped
//   EditorFavoritesSettings   - pinned asset guids (EditorContext favorites)
//   EditorOpenPagesSettings   - open page instance guids + the active one
//
// Other modules add their own sections to the SAME store (e.g. the material page's preview
// prefs) through EditorContext::ProjectEditorSettings(); their types must be registered
// before the app loads the store (registerEditors runs first, so per-subsystem registration
// belongs in each RegisterXxxEditor).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.app:layout;

import draconic.foundation;
import draconic.vfs;
import draconic.settings;
import draconic.xml.serialization;
import draconic.editor.core; // EditorContext (favorites)
import draconic.ui.toolkit;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    namespace ui = draconic::ui;
    namespace settings = draconic::settings;

    // The one per-project editor-state file (hand-editable XML, like every settings store).
    inline constexpr StringView kProjectEditorSettingsFile = u8"editor.project.settings.xml";

    // Bidirectional field walk of one dock node (children recurse via presence flags).
    inline void SerializeLayoutNode(ISerializer& ar, ui::toolkit::DockLayoutNode& node)
    {
        ar.BeginObject();
        draconic::foundation::Serialize(ar, "type", node.Type);
        draconic::foundation::Serialize(ar, "direction", node.Direction);
        draconic::foundation::Serialize(ar, "ratio", node.SplitRatio);
        draconic::foundation::Serialize(ar, "activeTab", node.ActiveTabIndex);
        draconic::foundation::Serialize(ar, "panels", node.PanelIds);

        bool hasFirst = static_cast<bool>(node.First);
        bool hasSecond = static_cast<bool>(node.Second);
        draconic::foundation::Serialize(ar, "hasFirst", hasFirst);
        draconic::foundation::Serialize(ar, "hasSecond", hasSecond);
        if (hasFirst)
        {
            if (ar.Mode() == SerializeMode::Read)
            {
                node.First = MakeUnique<ui::toolkit::DockLayoutNode>(DefaultAllocator());
            }
            ar.Key("first");
            SerializeLayoutNode(ar, *node.First);
        }
        if (hasSecond)
        {
            if (ar.Mode() == SerializeMode::Read)
            {
                node.Second = MakeUnique<ui::toolkit::DockLayoutNode>(DefaultAllocator());
            }
            ar.Key("second");
            SerializeLayoutNode(ar, *node.Second);
        }
        ar.EndObject();
    }

    // The dock-tree section: owns a DockLayoutNode snapshot (absent root = never captured).
    class EditorDockLayoutSettings final : public ISerializable
    {
        DRACONIC_OBJECT(EditorDockLayoutSettings, ISerializable)
    public:
        UniquePtr<ui::toolkit::DockLayoutNode> root;

        void Serialize(ISerializer& ar) override
        {
            bool hasRoot = static_cast<bool>(root);
            draconic::foundation::Serialize(ar, "hasRoot", hasRoot);
            if (hasRoot)
            {
                if (ar.Mode() == SerializeMode::Read)
                {
                    root = MakeUnique<ui::toolkit::DockLayoutNode>(DefaultAllocator());
                }
                ar.Key("root");
                SerializeLayoutNode(ar, *root);
            }
        }
    };

    class EditorFavoritesSettings final : public ISerializable
    {
        DRACONIC_OBJECT(EditorFavoritesSettings, ISerializable)
    public:
        Array<Guid> favorites;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "favorites", favorites);
        }
    };

    class EditorOpenPagesSettings final : public ISerializable
    {
        DRACONIC_OBJECT(EditorOpenPagesSettings, ISerializable)
    public:
        Array<Guid> pages;
        Guid active;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "pages", pages);
            ar.Key("active");
            ar.GuidValue(active);
        }
    };

    // Register the app-side section types (call once at startup, before any store Load).
    inline void RegisterEditorProjectSettingsTypes()
    {
        GlobalTypeRegistry().Register(EditorDockLayoutSettings::StaticType(),
                                      TypeDomain(u8"Editor"));
        RegisterSerializable<EditorDockLayoutSettings>();
        GlobalTypeRegistry().Register(EditorFavoritesSettings::StaticType(),
                                      TypeDomain(u8"Editor"));
        RegisterSerializable<EditorFavoritesSettings>();
        GlobalTypeRegistry().Register(EditorOpenPagesSettings::StaticType(),
                                      TypeDomain(u8"Editor"));
        RegisterSerializable<EditorOpenPagesSettings>();
    }

    // ---- store <-> live state ------------------------------------------------------------

    // Snapshot `dock`'s current layout into the store. NotFound when the dock tree is empty.
    [[nodiscard]] inline Status CaptureDockLayout(ui::toolkit::DockManager& dock,
                                                  settings::Settings& store)
    {
        UniquePtr<ui::toolkit::DockLayoutNode> layout = dock.ExportLayout();
        if (!layout)
        {
            return Status{ErrorCode::NotFound};
        }
        store.Section<EditorDockLayoutSettings>().root = Move(layout);
        store.MarkChanged<EditorDockLayoutSettings>();
        return Status{};
    }

    // Rebuild `dock` from the store's snapshot (panels matched by PersistenceId).
    // NotFound when no snapshot was ever captured (caller keeps its default layout).
    [[nodiscard]] inline Status ApplyDockLayout(ui::toolkit::DockManager& dock,
                                                settings::Settings& store)
    {
        const EditorDockLayoutSettings* section = store.Find<EditorDockLayoutSettings>();
        if (section == nullptr || !section->root)
        {
            return Status{ErrorCode::NotFound};
        }
        dock.ApplyLayout(section->root.Get());
        return Status{};
    }

    inline void CaptureFavorites(draconic::editor::EditorContext& context,
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

    inline void ApplyFavorites(draconic::editor::EditorContext& context,
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
        vfs::NativeFileSystem root(directory);
        UniquePtr<IStream> stream = root.Open(kProjectEditorSettingsFile, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        return store.Load(*stream, draconic::xml::XmlSerializerFactory());
    }

    // Persist the per-project store; on success, delete the pre-unification bespoke files
    // (layout.xml / favorites.bin / pages.bin) so stale state can't shadow the store.
    [[nodiscard]] inline Status SaveProjectEditorSettings(const settings::Settings& store,
                                                          StringView directory)
    {
        MemoryStream buffer;
        if (Status s = store.Save(buffer, draconic::xml::XmlSerializerFactory()); !s.IsOk())
        {
            return s;
        }
        vfs::NativeFileSystem root(directory);
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

    DRACONIC_DEFINE_OBJECT_VERSIONED(EditorDockLayoutSettings, "draconic::editor::app", 1)
    DRACONIC_DEFINE_OBJECT_VERSIONED(EditorFavoritesSettings, "draconic::editor::app", 1)
    DRACONIC_DEFINE_OBJECT_VERSIONED(EditorOpenPagesSettings, "draconic::editor::app", 1)
}
