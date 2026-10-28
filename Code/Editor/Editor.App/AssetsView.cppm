// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :assets_view partition
//
// The Assets panel (Documentation/Specs/editor-gui.md, 5.2, layer 7), over its markup
// (Data/Editor/Assets.gml) as the mock draws it: the project's source content, the content database
// the truth. The group tree on the left (folders, Content at the top); the toolbar's breadcrumb
// where it is; the group's subgroups then its assets on the right, as tiles or a list (kept per
// project), each by its type's glyph (its category's tint), its name, what it is, its cook badge
// and a star when a favourite. The search looks in the group shown and every group under it
// (Content: the whole project), by name or a guid's start, each match saying where it is, and is
// kept when another group is chosen, so a search narrows by picking where to look; the type filter keeps one type. With nothing to show it says what is
// so and offers the way on (clear the search, show every type). Enter or a double click opens an
// asset or enters a group; Reveal shows an asset where it lives. A drag of the selection carries
// its assets (AssetDrag) to a slot. It builds again when the database's shape moves, however the
// change came, and when a cook moves the badges.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:assets_view;

import foundation.core;
import foundation.content;
import foundation.settings;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import :asset_drag_data;
import :layout; // EditorAssetBrowserSettings
import :asset_operations;
import pipeline.core; // AssetCreator (New)

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace content = foundation::content;

    /// One asset or group as the browser draws it: a tile's picture over its name, or a list row's
    /// glyph, name, what it is, badge, Always Export and star. Its class (`tile` or `row`) says which.
    class AssetCell final : public gui::UIWidget
    {
        RTTI_OBJECT(AssetCell, gui::UIWidget)
    public:
        struct Shown
        {
            String Glyph;
            String GlyphClass;
            String Name;
            String Kind;
            String Badge;      ///< "Cooked", "Stale", "Failed"; empty: none
            String BadgeClass; ///< success, warning, danger
            bool Favourite = false;
            bool AlwaysExported = false; ///< an export takes it (or all a group holds) whatever reaches it
            bool Group = false;
            bool StateColumns = true; ///< a row's badge and Always Export columns: the browser's; a picker's rows show neither
            RefPtr<gui::Drawable> Picture; ///< its thumbnail, in its glyph's place (a tile's and a row's); null: none yet
        };

        explicit AssetCell(bool tile) : m_tile(tile)
        {
            AddClass(tile ? StringView(u8"asset-tile") : StringView(u8"asset-row"));
            gui::Node* glyphHolder = this;
            if (tile)
            {
                m_thumb = Part<gui::Panel>(*this, u8"asset-thumb");
                glyphHolder = m_thumb.Get();
            }
            m_glyph = Part<gui::Image>(*glyphHolder, tile ? StringView(u8"asset-glyph") : StringView(u8"row-icon"));
            if (tile)
            {
                m_picture = Part<gui::Image>(*m_thumb, u8"asset-picture");
                m_picture->SetDisplayed(false);
            }
            // A row's name and kind share a baseline, the kind at the end, right-aligned (each
            // row's ends in the same place, its badge and star in columns after it).
            gui::Node* textHolder = this;
            if (!tile)
            {
                m_text = Part<gui::Panel>(*this, u8"asset-row-text");
                textHolder = m_text.Get();
            }
            m_name = Part<gui::Label>(*textHolder, tile ? StringView(u8"asset-name") : StringView(u8"row-name"));
            m_kind = Part<gui::Label>(*textHolder, u8"row-kind");
            m_kind->SetDisplayed(!tile);
            m_badge = Part<gui::Label>(tile ? *m_thumb : static_cast<gui::Node&>(*this), u8"badge");
            // Always Export: a mark in a tile's corner; a row's icon in a column of its own, lit
            // when it is so, greyed when not, as its star is (every row shows both, so each
            // column lines up).
            if (tile)
            {
                m_exportMark = Part<gui::Panel>(*m_thumb, u8"export-mark");
                m_exportMark->SetTooltip(u8"Always exported");
                m_exportMark->SetHitTestVisible(true); // its tooltip
            }
            else
            {
                m_exportIcon = Part<gui::Image>(*this, u8"badge-icon");
                m_exportIcon->AddClass(u8"always-export"); // not `export`: the export dialog's class
                m_exportIcon->SetSource(String(u8"Editor/Icons/export.svg"));
                m_exportIcon->SetHitTestVisible(true); // its tooltip
            }
            m_star = Part<gui::Image>(tile ? *m_thumb : static_cast<gui::Node&>(*this), u8"badge-icon");
            m_star->AddClass(u8"star");
            m_star->SetSource(String(u8"Editor/Icons/star.svg"));
            m_star->SetTooltip(u8"Favourite");
            if (!tile)
                m_star->SetHitTestVisible(true); // its tooltip
        }
        void Show(const Shown& shown)
        {
            if (!m_glyphClass.IsEmpty())
                m_glyph->RemoveClass(m_glyphClass.AsView());
            m_glyphClass = shown.GlyphClass;
            if (!m_glyphClass.IsEmpty())
                m_glyph->AddClass(m_glyphClass.AsView());
            // A tile shows the asset's picture once there is one, its type's glyph till then; a
            // row, the picture small in its icon's place.
            const bool pictured = m_picture && shown.Picture;
            if (m_picture)
            {
                m_picture->SetDrawable(shown.Picture);
                m_picture->SetDisplayed(pictured);
            }
            if (!m_tile && shown.Picture)
            {
                m_glyph->SetDrawable(shown.Picture);
                m_glyph->AddClass(u8"row-picture");
            }
            else
            {
                m_glyph->RemoveClass(u8"row-picture");
                m_glyph->SetSource(shown.Glyph);
            }
            m_glyph->SetDisplayed(!pictured);
            m_name->SetText(shown.Name.AsView());
            m_kind->SetText(shown.Kind.AsView());
            if (!m_badgeClass.IsEmpty())
                m_badge->RemoveClass(m_badgeClass.AsView());
            m_badgeClass = shown.BadgeClass;
            if (!m_badgeClass.IsEmpty())
                m_badge->AddClass(m_badgeClass.AsView());
            m_badge->SetText(shown.Badge.AsView());
            // A tile marks only what needs a look (stale, failed); a row says each state, its badge
            // and star keeping their columns when it has none.
            if (m_tile)
            {
                m_badge->SetDisplayed(!shown.Badge.IsEmpty() && shown.BadgeClass != StringView(u8"success"));
                m_star->SetDisplayed(shown.Favourite);
                m_exportMark->SetDisplayed(shown.AlwaysExported);
            }
            else
            {
                m_badge->SetVisible(!shown.Badge.IsEmpty());
                m_badge->SetDisplayed(shown.StateColumns);
                m_exportIcon->SetDisplayed(shown.StateColumns);
                Lit(*m_exportIcon, shown.AlwaysExported);
                m_exportIcon->SetTooltip(shown.AlwaysExported ? StringView(u8"Always exported") : StringView(u8"Exported when something it ships uses it"));
                Lit(*m_star, shown.Favourite);
                m_star->SetTooltip(shown.Favourite ? StringView(u8"Favourite") : StringView(u8"Not a favourite"));
            }
            if (shown.Group)
                AddClass(u8"group");
            else
                RemoveClass(u8"group");
            SetTooltip(!m_tile ? StringView{} : shown.Kind.IsEmpty() ? shown.Name.AsView() : Format(u8"{} ({})", shown.Name, shown.Kind).AsView());
        }
        [[nodiscard]] StringView Name() const { return m_name->GetText(); }
        [[nodiscard]] StringView Kind() const { return m_kind->GetText(); }
        [[nodiscard]] StringView Badge() const { return m_badge->GetText(); }
        /// A tile's star shows; a row's is lit.
        [[nodiscard]] bool ShowsStar() const { return m_tile ? m_star->IsDisplayed() : m_star->HasClass(u8"on"); }
        [[nodiscard]] bool ShowsAlwaysExport() const
        {
            return m_tile ? m_exportMark->IsDisplayed() : m_exportIcon->HasClass(u8"on");
        }
        [[nodiscard]] bool IsTile() const noexcept { return m_tile; }
        [[nodiscard]] bool ShowsPicture() const
        {
            return m_tile ? m_picture && m_picture->IsDisplayed() && m_picture->GetDrawable() != nullptr : m_glyph->HasClass(u8"row-picture");
        }
        [[nodiscard]] gui::Label* NameLabel() const noexcept { return m_name.Get(); }
        [[nodiscard]] gui::Label* KindLabel() const noexcept { return m_kind.Get(); }

    private:
        static void Lit(gui::Image& icon, bool on)
        {
            if (on)
                icon.AddClass(u8"on");
            else
                icon.RemoveClass(u8"on");
        }
        template <typename T>
        RefPtr<T> Part(gui::Node& into, StringView cls)
        {
            RefPtr<T> part = MakeRef<T>(MemoryAllocator());
            part->AddClass(cls);
            part->SetHitTestVisible(false);
            into.AddChild(part.Get());
            return part;
        }

        bool m_tile;
        RefPtr<gui::Panel> m_thumb;
        RefPtr<gui::Image> m_picture; // a tile's thumbnail
        RefPtr<gui::Panel> m_text; // a row's name and kind
        RefPtr<gui::Image> m_exportIcon;  // a row's Always Export
        RefPtr<gui::Panel> m_exportMark;  // a tile's
        RefPtr<gui::Image> m_glyph;
        RefPtr<gui::Label> m_name;
        RefPtr<gui::Label> m_kind;
        RefPtr<gui::Label> m_badge;
        RefPtr<gui::Image> m_star;
        String m_glyphClass;
        String m_badgeClass;
    };

    class AssetsView
    {
    public:
        /// Its markup, under the data root.
        static constexpr const char8_t* kPath = u8"Editor/Assets.gml";
        static constexpr StringView kTopGroup = u8"Content";
        /// What an asset type (by name) is called to the user: its displayName, else its name. The
        /// asset picker's rows say it as the browser's do.
        [[nodiscard]] static String KindOf(StringView typeName)
        {
            for (const TypeInfo* type : GlobalTypeRegistry().All())
                if (type != nullptr && type->name != nullptr && StringView(reinterpret_cast<const utf8char*>(type->name)) == typeName)
                    return String(TypeAttrString(*type, "displayName", typeName));
            return String(typeName);
        }

        /// What it works over.
        struct Services
        {
            editor::EditorContext* context = nullptr;   // the project, favourites, the project's editor store
            editor::EditorCookService* cook = nullptr;  // the badges; null: none
            editor::EditorIcons* icons = nullptr;       // a type's glyph; null: generic ones
            Function<void(content::Instance&)> open;    // an asset opened (its page)
            AssetOperations* operations = nullptr;      // rename, duplicate, delete, cook; null: no menus
            /// An asset's picture (the thumbnails'), null while there is none; unset: glyphs only.
            /// A picture that arrives later is told through the context's thumbnail listeners.
            Function<RefPtr<gui::Drawable>(const Guid&)> picture;
            /// Asks for files to import into a group (the OS's picker); unset: no Import....
            Function<void(content::Group&)> import;
            /// Runs a creator into a group (File > New's create-and-open); unset: no New menu.
            Function<void(const pipeline::AssetCreator&, content::Group*)> create;
        };

        AssetsView(IAllocator& allocator, Services services, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_services(Move(services)), m_content(Move(content)), m_rows(allocator), m_treeGroups(allocator), m_model(*this)
        {
            if (!m_content)
                return;
            AssetsView* self = this;
            m_tree = Find<gui::TreeView>(u8"assets-groups");
            if (m_tree != nullptr)
            {
                m_tree->SetModel(&m_groupModel);
                m_tree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                              {
                                                  if (self->m_building || !index.IsValid())
                                                      return;
                                                  const i64 node = index.InternalId;
                                                  if (node >= 0 && static_cast<usize>(node) < self->m_treeGroups.Size())
                                                      self->Navigate(*self->m_treeGroups[static_cast<usize>(node)]);
                                              });
            }
            m_list = Find<gui::ListView>(u8"assets-content");
            if (m_list != nullptr)
            {
                gui::RowFactory rows;
                rows.TypeOf = [self](const gui::ModelIndex&) { return self->m_tiles ? 0u : 1u; };
                rows.Create = [self](u32 type) -> RefPtr<gui::Node> { return MakeRef<AssetCell>(self->m_allocator, type == 0u); };
                rows.Bind = [self](gui::Node& cell, const gui::ModelIndex& index) { self->BindCell(cell, index.Row); };
                m_list->SetRowFactory(Move(rows));
                m_list->SetModel(&m_model);
                m_list->SetOnActivated([self](gui::ModelIndex index) { self->OpenRow(index.Row); });
                m_list->SetDragType(String(AssetDrag::kType));
                m_list->SetOnDragPayload([self](gui::DragPayload& payload, Span<const gui::ItemId> ids) { self->DragPayloadFor(payload, ids); });
                // A row's menu, or the group's where no row is; Delete asks to delete the selection.
                (void)m_list->AddEventListener(gui::EventType::ContextMenu, [self](const gui::Event& e)
                                               {
                                                   const Float2 at = static_cast<const gui::MouseEvent&>(e).Position;
                                                   self->ShowMenuAt(*self->m_list, at, self->m_list->ItemAt(at));
                                                   e.Handled = true;
                                               });
                (void)m_list->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                               {
                                                   const auto& key = static_cast<const gui::KeyEvent&>(e);
                                                   if (static_cast<gui::KeyCode>(key.KeyCode) == gui::KeyCode::Delete && key.Modifiers == 0)
                                                   {
                                                       self->DeleteSelection();
                                                       e.Handled = true;
                                                   }
                                               });
            }
            if (m_tree != nullptr)
            {
                // The tree's: the group it shows (a right press selects the group under it first).
                (void)m_tree->AddEventListener(gui::EventType::ContextMenu, [self](const gui::Event& e)
                                               {
                                                   if (self->m_current != nullptr)
                                                       self->ShowGroupMenu(*self->m_tree, static_cast<const gui::MouseEvent&>(e).Position, *self->m_current, true);
                                                   e.Handled = true;
                                               });
                (void)m_tree->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                               {
                                                   const auto& key = static_cast<const gui::KeyEvent&>(e);
                                                   if (static_cast<gui::KeyCode>(key.KeyCode) == gui::KeyCode::Delete && key.Modifiers == 0 &&
                                                       self->m_current != nullptr)
                                                   {
                                                       self->ConfirmDeleteGroup(*self->m_current);
                                                       e.Handled = true;
                                                   }
                                               });
            }
            if (auto* state = Find<gui::UIWidget>(u8"assets-state"))
                (void)state->AddEventListener(gui::EventType::ContextMenu, [self, state](const gui::Event& e)
                                              {
                                                  self->ShowMenuAt(*state, static_cast<const gui::MouseEvent&>(e).Position, -1);
                                                  e.Handled = true;
                                              });
            m_crumbs = Find<gui::BreadcrumbBar>(u8"assets-crumbs");
            if (m_crumbs != nullptr)
            {
                m_crumbs->SetOnNavigate([self](const gui::BreadcrumbItem& item)
                                        {
                                            if (content::Group* group = self->GroupAt(item.Id.AsView()))
                                                self->Navigate(*group);
                                        });
                // Assets dropped on a segment move up into that group.
                m_crumbs->SetSegmentDropTypes(AssetDrag::kType);
                m_crumbs->SetOnSegmentDrop([self](const gui::BreadcrumbItem& item, const gui::DragPayload& payload, gui::DropEffect)
                                           {
                                               if (content::Group* group = self->GroupAt(item.Id.AsView()))
                                                   (void)self->Take(payload, *group);
                                           });
            }
            if (auto* search = Find<gui::TextField>(u8"assets-search"))
                search->SetOnTextChanging([self](StringView text) { self->Search(text); });
            if (auto* clear = Find<gui::Button>(u8"assets-clear-search"))
                clear->SetOnClick([self]() { self->SetSearch(StringView{}); });
            if (auto* types = Find<gui::Button>(u8"assets-types"))
                types->SetOnClick([self]() { self->ShowTypeMenu(); });
            if (auto* tiles = Find<gui::Button>(u8"assets-tiles"))
                tiles->SetOnClick([self]() { self->SetTiles(true); });
            if (auto* list = Find<gui::Button>(u8"assets-list"))
                list->SetOnClick([self]() { self->SetTiles(false); });
            if (auto* verb = Find<gui::Button>(u8"assets-state-verb"))
                verb->SetOnClick([self]() { self->StateVerb(); });
            ShowLayout();
            // A picture that arrives redraws the cells (the one showing it among them).
            if (m_services.context != nullptr && m_services.picture)
                m_pictureListener = m_services.context->AddThumbnailListener([self](const Guid&) { self->m_model.Changed(); });
        }
        AssetsView(const AssetsView&) = delete;
        AssetsView& operator=(const AssetsView&) = delete;
        ~AssetsView()
        {
            if (m_pictureListener != 0)
                m_services.context->RemoveThumbnailListener(m_pictureListener);
            if (m_list != nullptr)
                m_list->SetModel(nullptr);
            if (m_tree != nullptr)
                m_tree->SetModel(nullptr);
        }

        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }

        /// Once a frame: builds again when the project, its database's shape or the badges moved.
        void Refresh()
        {
            content::ContentDatabase* db = Sources();
            const u64 revision = db != nullptr ? db->Revision() : 0;
            const u64 cooked = m_services.cook != nullptr ? m_services.cook->Revision() : 0;
            if (db != m_db || revision != m_dbRevision)
                Rebuild();
            else if (cooked != m_cookRevision)
            {
                m_cookRevision = cooked;
                m_model.Changed(); // the badges
            }
        }
        /// Builds the tree and the content again from the database, keeping where it is, the
        /// selection and the scroll where they still are.
        void Rebuild()
        {
            content::ContentDatabase* db = Sources();
            if (db != m_db)
            {
                // Another project (or none): back to its top, as it opens, in its own view.
                m_db = db;
                m_current = nullptr;
                m_search = String{};
                m_type = String{};
                if (auto* search = Find<gui::TextField>(u8"assets-search"))
                    search->SetText(StringView{});
                LoadTiles();
            }
            m_dbRevision = db != nullptr ? db->Revision() : 0;
            m_cookRevision = m_services.cook != nullptr ? m_services.cook->Revision() : 0;
            if (m_current == nullptr || !Contains(m_current))
                m_current = db != nullptr ? db->RootGroup() : nullptr;
            BuildTree();
            BuildRows();
        }

        // === Where it is ===
        [[nodiscard]] content::Group* CurrentGroup() const noexcept { return m_current; }
        /// Shows `group`'s content (with a search, its matches in it and under it: the search
        /// is kept), its tree row selected, the breadcrumb following.
        void Navigate(content::Group& group)
        {
            if (!Contains(&group))
                return;
            m_current = &group;
            SelectTreeGroup(group);
            BuildRows();
        }
        /// Shows the asset `id` where it lives: its group, it selected and in view. False when
        /// the project has no such asset.
        bool Reveal(const Guid& id)
        {
            content::ContentDatabase* db = Sources();
            content::Instance* instance = db != nullptr ? db->GetInstance(id) : nullptr;
            if (instance == nullptr)
                return false;
            m_type = String{};
            if (!m_search.IsEmpty())
                SetSearch(StringView{}); // where it lives, not among a search's matches
            Navigate(instance->OwningGroup());
            for (usize row = 0; row < m_rows.Size(); ++row)
                if (m_rows[row].instance == instance && m_list != nullptr)
                    m_list->SetSelectedItem(static_cast<i32>(row));
            return true;
        }

        // === What it shows ===
        /// Looks in every group for assets whose name holds `text` (case folded) or whose guid
        /// starts with it; empty shows the group again.
        void SetSearch(StringView text)
        {
            if (auto* search = Find<gui::TextField>(u8"assets-search"))
                search->SetText(text);
            Search(text);
        }
        [[nodiscard]] StringView SearchText() const noexcept { return m_search.AsView(); }
        /// Keeps only assets of the type named `typeName`; empty: every type.
        void SetTypeFilter(StringView typeName)
        {
            m_type = String(typeName);
            if (auto* types = Find<gui::Button>(u8"assets-types"))
            {
                if (m_type.IsEmpty())
                    types->RemoveClass(u8"checked");
                else
                    types->AddClass(u8"checked");
                types->SetTooltip(m_type.IsEmpty() ? StringView(u8"Filter by type") : Format(u8"Showing {} only", KindOf(m_type.AsView())).AsView());
            }
            BuildRows();
        }
        [[nodiscard]] StringView TypeFilter() const noexcept { return m_type.AsView(); }
        /// Tiles or a list, kept with the project.
        void SetTiles(bool tiles)
        {
            if (m_tiles == tiles)
                return;
            m_tiles = tiles;
            if (foundation::settings::Settings* store = m_services.context != nullptr ? m_services.context->ProjectEditorSettings() : nullptr)
            {
                store->Section<EditorAssetBrowserSettings>().gridMode = tiles;
                store->MarkChanged<EditorAssetBrowserSettings>();
                m_services.context->RequestProjectEditorSettingsSave();
            }
            ShowLayout();
        }
        [[nodiscard]] bool ShowsTiles() const noexcept { return m_tiles; }

        /// The rows shown: a group's or an asset's, by their place.
        [[nodiscard]] usize RowCount() const noexcept { return m_rows.Size(); }
        [[nodiscard]] StringView RowName(usize row) const
        {
            if (row >= m_rows.Size())
                return {};
            return m_rows[row].group != nullptr ? m_rows[row].group->Name() : m_rows[row].instance->Name();
        }
        [[nodiscard]] content::Group* RowGroup(usize row) const { return row < m_rows.Size() ? m_rows[row].group : nullptr; }
        [[nodiscard]] content::Instance* RowAsset(usize row) const { return row < m_rows.Size() ? m_rows[row].instance : nullptr; }
        /// Opens an asset's page, or enters a group (Enter, a double click).
        void OpenRow(i32 row)
        {
            if (row < 0 || static_cast<usize>(row) >= m_rows.Size())
                return;
            if (content::Group* group = m_rows[static_cast<usize>(row)].group)
                Navigate(*group);
            else if (content::Instance* instance = m_rows[static_cast<usize>(row)].instance; instance != nullptr && m_services.open)
                m_services.open(*instance);
        }
        /// The selected assets, in their order (groups left out).
        [[nodiscard]] Array<content::Instance*> SelectedAssets() const
        {
            Array<content::Instance*> out;
            if (m_list != nullptr)
                for (usize row = 0; row < m_rows.Size(); ++row)
                    if (m_rows[row].instance != nullptr && m_list->IsItemSelected(static_cast<i32>(row)))
                        out.PushBack(m_rows[row].instance);
            return out;
        }

        [[nodiscard]] bool ShowsState() const
        {
            const auto* state = Find<gui::UIWidget>(u8"assets-state");
            return state != nullptr && state->IsDisplayed();
        }
        [[nodiscard]] StringView StateTitle() const
        {
            const auto* title = Find<gui::Label>(u8"assets-state-title");
            return title != nullptr ? title->GetText() : StringView{};
        }
        [[nodiscard]] gui::ListView* List() const noexcept { return m_list; }
        [[nodiscard]] gui::TreeView* Tree() const noexcept { return m_tree; }
        [[nodiscard]] gui::BreadcrumbBar* Crumbs() const noexcept { return m_crumbs; }
        /// The open menu (the type filter's, a row's, the group's), for a test.
        [[nodiscard]] gui::Menu* TypeMenu() const noexcept { return m_menu.Get(); }
        [[nodiscard]] gui::Menu* OpenMenu() const noexcept { return m_menu && m_menu->IsOpen() ? m_menu.Get() : nullptr; }
        [[nodiscard]] gui::MessageBox* OpenConfirm() const noexcept { return m_confirm && m_confirm->IsOpen() ? m_confirm.Get() : nullptr; }
        /// The menu `row` would show (-1: the group's own), opened at the list's corner.
        void ShowMenuFor(i32 row)
        {
            if (m_list != nullptr)
                ShowMenuAt(*m_list, m_list->ConvertToWorldSpace(Float2{4.0f, 4.0f}), row);
        }
        /// Asks to delete the selected assets, or the selected group (Delete).
        void DeleteSelection()
        {
            if (m_services.operations == nullptr || m_list == nullptr)
                return;
            Array<Guid> ids;
            for (content::Instance* instance : SelectedAssets())
                ids.PushBack(instance->Id());
            if (!ids.IsEmpty())
            {
                ConfirmDelete(Move(ids));
                return;
            }
            const i32 row = m_list->GetSelectedItem();
            if (content::Group* group = RowGroup(row >= 0 ? static_cast<usize>(row) : m_rows.Size()))
                ConfirmDeleteGroup(*group);
        }

    private:
        struct Row
        {
            content::Group* group = nullptr;       // a subgroup's row
            content::Instance* instance = nullptr; // an asset's
        };
        static constexpr u64 kGroupBit = 1ull << 62;

        // The rows as the list's model: an asset's id its guid's, a group's its path's.
        class Model final : public gui::IModel
        {
        public:
            explicit Model(AssetsView& view) : m_view(view) {}
            [[nodiscard]] usize RowCount(const gui::ModelIndex& parent = {}) const override { return parent.IsValid() ? 0 : m_view.m_rows.Size(); }
            [[nodiscard]] usize ColumnCount() const override { return 1; }
            [[nodiscard]] gui::Variant Data(const gui::ModelIndex& index, gui::ModelRole role = gui::ModelRole::Display) const override
            {
                return role == gui::ModelRole::Display && index.Row >= 0 ? gui::Variant(m_view.RowName(static_cast<usize>(index.Row))) : gui::Variant{};
            }
            [[nodiscard]] gui::ItemId IdOf(const gui::ModelIndex& index) const override
            {
                if (index.Row < 0 || static_cast<usize>(index.Row) >= m_view.m_rows.Size())
                    return gui::kNoItem;
                return IdOfRow(m_view.m_rows[static_cast<usize>(index.Row)]);
            }
            [[nodiscard]] gui::ModelIndex IndexOfId(gui::ItemId id) const override
            {
                for (usize row = 0; row < m_view.m_rows.Size(); ++row)
                    if (IdOfRow(m_view.m_rows[row]) == id)
                        return gui::ModelIndex{static_cast<i32>(row), 0};
                return {};
            }
            [[nodiscard]] static gui::ItemId IdOfRow(const Row& row)
            {
                if (row.group != nullptr)
                    return (Hash<StringView>{}(row.group->Path().AsView()) & ~(kGroupBit | (kGroupBit << 1))) | kGroupBit;
                const Guid& id = row.instance->Id();
                return (id.high ^ (id.low * 31u)) & ~(kGroupBit | (kGroupBit << 1));
            }
            // A row's name is edited in place (F2, the menu's Rename): the asset or group renamed.
            [[nodiscard]] bool IsEditable(const gui::ModelIndex& index) const override
            {
                return m_view.m_services.operations != nullptr && index.Row >= 0 && static_cast<usize>(index.Row) < m_view.m_rows.Size();
            }
            bool SetData(const gui::ModelIndex& index, const gui::Variant& value) override
            {
                return index.Row >= 0 && m_view.RenameRow(static_cast<usize>(index.Row), value.AsString());
            }
            // Assets dropped onto a group's row move into it.
            [[nodiscard]] gui::DropEffect CanDrop(const gui::ModelDrop& drop, String& reason) const override
            {
                content::Group* group = drop.Place == gui::DropPlace::On && drop.Index.Row >= 0 ? m_view.RowGroup(static_cast<usize>(drop.Index.Row)) : nullptr;
                return group != nullptr && drop.Payload != nullptr && m_view.CanTake(*drop.Payload, *group, reason) ? gui::DropEffect::Move : gui::DropEffect::None;
            }
            bool Drop(const gui::ModelDrop& drop) override
            {
                content::Group* group = drop.Place == gui::DropPlace::On && drop.Index.Row >= 0 ? m_view.RowGroup(static_cast<usize>(drop.Index.Row)) : nullptr;
                return group != nullptr && drop.Payload != nullptr && m_view.Take(*drop.Payload, *group);
            }
            void Reset() { NotifyReset(); }
            void Changed()
            {
                if (!m_view.m_rows.IsEmpty())
                    NotifyRowsChanged(gui::ModelIndex{}, 0, static_cast<i32>(m_view.m_rows.Size()));
            }

        private:
            AssetsView& m_view;
        };
        // The group tree's model: a group (not the top one) renamed in place.
        class GroupTreeModel final : public gui::TreeModel
        {
        public:
            explicit GroupTreeModel(AssetsView& view) : m_view(view) {}
            [[nodiscard]] bool IsEditable(const gui::ModelIndex& index) const override
            {
                const content::Group* group = m_view.TreeGroupAt(index);
                return m_view.m_services.operations != nullptr && group != nullptr && group->Parent() != nullptr;
            }
            bool SetData(const gui::ModelIndex& index, const gui::Variant& value) override
            {
                content::Group* group = m_view.TreeGroupAt(index);
                return group != nullptr && m_view.RenameGroup(*group, value.AsString());
            }
            // Assets dropped onto a group move into it.
            [[nodiscard]] gui::DropEffect CanDrop(const gui::ModelDrop& drop, String& reason) const override
            {
                content::Group* group = drop.Place == gui::DropPlace::On ? m_view.TreeGroupAt(drop.Index) : nullptr;
                return group != nullptr && drop.Payload != nullptr && m_view.CanTake(*drop.Payload, *group, reason) ? gui::DropEffect::Move : gui::DropEffect::None;
            }
            bool Drop(const gui::ModelDrop& drop) override
            {
                content::Group* group = drop.Place == gui::DropPlace::On ? m_view.TreeGroupAt(drop.Index) : nullptr;
                return group != nullptr && drop.Payload != nullptr && m_view.Take(*drop.Payload, *group);
            }

        private:
            AssetsView& m_view;
        };

        [[nodiscard]] content::ContentDatabase* Sources() const
        {
            editor::EditorProject* project = m_services.context != nullptr ? m_services.context->Project() : nullptr;
            return project != nullptr ? &project->SourceDb() : nullptr;
        }
        [[nodiscard]] bool Contains(const content::Group* group) const
        {
            content::ContentDatabase* db = Sources();
            if (db == nullptr || group == nullptr)
                return false;
            Array<content::Group*> pending;
            pending.PushBack(db->RootGroup());
            while (!pending.IsEmpty())
            {
                content::Group* at = pending.Back();
                pending.PopBack();
                if (at == group)
                    return true;
                for (content::Group* child : at->Groups())
                    pending.PushBack(child);
            }
            return false;
        }
        // A group by its breadcrumb id (its path; empty: the top).
        [[nodiscard]] content::Group* GroupAt(StringView path) const
        {
            content::ContentDatabase* db = Sources();
            content::Group* group = db != nullptr ? db->RootGroup() : nullptr;
            usize at = 0;
            while (group != nullptr && at < path.Size())
            {
                usize end = at;
                while (end < path.Size() && path[end] != u8'/')
                    ++end;
                group = group->GetGroup(path.SubStr(at, end - at));
                at = end + 1;
            }
            return group;
        }
        [[nodiscard]] static String ShownPath(const content::Group& group)
        {
            return group.Parent() == nullptr ? String(kTopGroup) : Format(u8"{}/{}", kTopGroup, group.Path());
        }

        // The type an asset's instance names, for its glyph and kind.
        [[nodiscard]] static const TypeInfo* TypeOfAsset(const content::Instance& instance)
        {
            return GlobalTypeRegistry().FindByName(reinterpret_cast<const char*>(String(instance.TypeNamespace()).CStr()),
                                                   reinterpret_cast<const char*>(String(instance.TypeName()).CStr()));
        }

        void Search(StringView text)
        {
            if (m_search.AsView() == text)
                return;
            m_search = String(text);
            if (auto* clear = Find<gui::Button>(u8"assets-clear-search"))
                clear->SetVisible(!m_search.IsEmpty());
            BuildRows();
        }
        [[nodiscard]] bool Matches(const content::Instance& instance) const
        {
            if (!m_type.IsEmpty() && instance.TypeName() != m_type.AsView())
                return false;
            if (m_search.IsEmpty())
                return true;
            if (instance.Name().ContainsIgnoreCase(m_search.AsView()))
                return true;
            // A guid's start (four characters or more: a guid pasted or typed).
            if (m_search.Size() < 4)
                return false;
            utf8char chars[37];
            instance.Id().ToChars(chars);
            const StringView guid(chars, 36);
            return guid.SubStr(0, Min(m_search.Size(), guid.Size())).ContainsIgnoreCase(m_search.AsView()) && m_search.Size() <= guid.Size();
        }

        // The group tree: a node per group, the top one Content, all open.
        void BuildTree()
        {
            if (m_tree == nullptr)
                return;
            m_building = true;
            m_groupModel.Clear();
            m_treeGroups.Clear();
            if (content::ContentDatabase* db = Sources())
                AddTreeGroup(*db->RootGroup(), gui::TreeModel::kRoot);
            for (usize node = 0; node < m_treeGroups.Size(); ++node)
                m_tree->SetIdExpanded(static_cast<gui::ItemId>(node), true);
            if (m_current != nullptr)
                SelectTreeGroup(*m_current);
            m_building = false;
        }
        void AddTreeGroup(content::Group& group, i32 parent)
        {
            const i32 node = m_groupModel.AddNode(parent, group.Parent() == nullptr ? kTopGroup : group.Name());
            m_groupModel.SetIcon(node, u8"Editor/Icons/folder.svg");
            while (m_treeGroups.Size() <= static_cast<usize>(node))
                m_treeGroups.PushBack(nullptr);
            m_treeGroups[static_cast<usize>(node)] = &group;
            for (content::Group* child : group.Groups())
                AddTreeGroup(*child, node);
        }
        void SelectTreeGroup(const content::Group& group)
        {
            if (m_tree == nullptr)
                return;
            for (usize node = 0; node < m_treeGroups.Size(); ++node)
                if (m_treeGroups[node] == &group)
                {
                    const bool building = m_building;
                    m_building = true; // a selection from code is no navigation
                    for (i32 item = 0; item < static_cast<i32>(m_tree->GetItemCount()); ++item)
                        if (m_tree->GetItemIndex(item).InternalId == static_cast<i64>(node))
                            m_tree->SetSelectedItem(item); // every group is open: each is an item
                    m_building = building;
                }
        }

        // The content: the group's subgroups, then its assets; a search's matches from every
        // group; then the breadcrumb and the state.
        void BuildRows()
        {
            m_rows.Clear();
            content::ContentDatabase* db = Sources();
            if (db != nullptr && m_current != nullptr)
            {
                if (!m_search.IsEmpty())
                    CollectMatches(*m_current);
                else
                {
                    if (m_type.IsEmpty())
                        for (content::Group* child : m_current->Groups())
                            m_rows.PushBack(Row{child, nullptr});
                    for (content::Instance* instance : m_current->Instances())
                        if (Matches(*instance))
                            m_rows.PushBack(Row{nullptr, instance});
                }
            }
            m_model.Reset();
            ShowCrumbs();
            ShowState();
        }
        void CollectMatches(content::Group& group)
        {
            for (content::Instance* instance : group.Instances())
                if (Matches(*instance))
                    m_rows.PushBack(Row{nullptr, instance});
            for (content::Group* child : group.Groups())
                CollectMatches(*child);
        }
        void ShowCrumbs()
        {
            if (m_crumbs == nullptr)
                return;
            Array<gui::BreadcrumbItem> items;
            Array<content::Group*> chain;
            for (content::Group* at = m_current; at != nullptr; at = at->Parent())
                chain.PushBack(at);
            for (usize i = chain.Size(); i > 0; --i)
            {
                content::Group& group = *chain[i - 1];
                gui::BreadcrumbItem item;
                item.Id = group.Path();
                item.Text = group.Parent() == nullptr ? String(kTopGroup) : String(group.Name());
                item.Tooltip = ShownPath(group);
                items.PushBack(Move(item));
            }
            m_crumbs->SetItems(items.AsSpan());
        }
        void ShowState()
        {
            const bool none = m_rows.IsEmpty();
            if (m_list != nullptr)
                m_list->SetDisplayed(!none);
            if (auto* state = Find<gui::UIWidget>(u8"assets-state"))
                state->SetDisplayed(none);
            if (!none)
                return;
            StringView icon = u8"Editor/Icons/folder-open.svg";
            String title;
            String text;
            String verb;
            if (Sources() == nullptr)
                title = String(u8"No project is open");
            else if (!m_search.IsEmpty())
            {
                icon = u8"Editor/Icons/search.svg";
                title = m_current != nullptr && m_current != Sources()->RootGroup() ? Format(u8"Nothing matches {} in {}", m_search, ShownPath(*m_current))
                                                                                     : Format(u8"Nothing matches {}", m_search);
                verb = String(u8"Clear the search");
            }
            else if (!m_type.IsEmpty())
            {
                icon = u8"Editor/Icons/filter.svg";
                title = Format(u8"No {} here", KindOf(m_type.AsView()));
                verb = String(u8"Show every type");
            }
            else
            {
                title = Format(u8"Nothing in {} yet", m_current != nullptr ? ShownPath(*m_current) : String(kTopGroup));
                text = String(u8"Create an asset or import files into it.");
            }
            if (auto* image = Find<gui::Image>(u8"assets-state-icon"))
                image->SetSource(String(icon));
            if (auto* label = Find<gui::Label>(u8"assets-state-title"))
                label->SetText(title.AsView());
            if (auto* label = Find<gui::Label>(u8"assets-state-text"))
            {
                label->SetText(text.AsView());
                label->SetDisplayed(!text.IsEmpty());
            }
            if (auto* button = Find<gui::Button>(u8"assets-state-verb"))
            {
                button->SetText(verb.AsView());
                button->SetDisplayed(!verb.IsEmpty());
            }
        }
        void StateVerb()
        {
            if (!m_search.IsEmpty())
                SetSearch(StringView{});
            else if (!m_type.IsEmpty())
                SetTypeFilter(StringView{});
        }

        // The cell for a row, as it shows now.
        void BindCell(gui::Node& node, i32 index)
        {
            auto* cell = Cast<AssetCell>(&node);
            if (cell == nullptr || index < 0 || static_cast<usize>(index) >= m_rows.Size())
                return;
            const Row& row = m_rows[static_cast<usize>(index)];
            AssetCell::Shown shown;
            if (row.group != nullptr)
            {
                shown.Group = true;
                shown.Glyph = String(u8"Editor/Icons/folder.svg");
                shown.Name = String(row.group->Name()); // its folder says what it is
                shown.AlwaysExported = m_services.operations != nullptr && m_services.operations->IsAlwaysExported(*row.group);
            }
            else
            {
                const content::Instance& instance = *row.instance;
                shown.Name = String(instance.Name());
                shown.Kind = KindOf(instance.TypeName());
                if (!m_search.IsEmpty())
                    shown.Kind = Format(u8"{} in {}", shown.Kind, ShownPath(instance.OwningGroup()));
                if (const TypeInfo* type = TypeOfAsset(instance); type != nullptr && m_services.icons != nullptr)
                {
                    const editor::TypeIcon icon = m_services.icons->For(*type, u8"Asset");
                    shown.Glyph = icon.Source;
                    shown.GlyphClass = icon.Class;
                }
                if (shown.Glyph.IsEmpty())
                    shown.Glyph = String(u8"Editor/Icons/file.svg");
                shown.Favourite = m_services.context != nullptr && m_services.context->IsFavorite(instance.Id());
                shown.AlwaysExported = m_services.operations != nullptr && m_services.operations->IsAlwaysExported(instance.Id());
                if (m_services.picture)
                    shown.Picture = m_services.picture(instance.Id());
                if (m_services.cook != nullptr)
                    switch (m_services.cook->BadgeFor(const_cast<content::Instance&>(instance)))
                    {
                    case editor::CookBadge::Cooked:
                        shown.Badge = String(u8"Cooked");
                        shown.BadgeClass = String(u8"success");
                        break;
                    case editor::CookBadge::Missing:
                        shown.Badge = String(u8"Stale");
                        shown.BadgeClass = String(u8"warning");
                        break;
                    case editor::CookBadge::Failed:
                        shown.Badge = String(u8"Failed");
                        shown.BadgeClass = String(u8"danger");
                        break;
                    case editor::CookBadge::NoBuilder:
                        break;
                    }
            }
            cell->Show(shown);
        }

        // Tiles or a list, the toggles saying which.
        void ShowLayout()
        {
            if (m_list != nullptr)
            {
                m_list->SetLayoutMode(m_tiles ? gui::ItemLayout::Grid : gui::ItemLayout::List);
                if (m_tiles)
                {
                    m_list->SetCellWidth(kTileWidth);
                    m_list->SetCellHeight(kTileHeight);
                    m_list->RemoveClass(u8"asset-list");
                    m_list->AddClass(u8"asset-tiles");
                }
                else
                {
                    m_list->RemoveClass(u8"asset-tiles");
                    m_list->AddClass(u8"asset-list");
                }
                m_model.Reset(); // the rows' kind follows
            }
            if (auto* tiles = Find<gui::Button>(u8"assets-tiles"))
            {
                if (m_tiles)
                    tiles->AddClass(u8"checked");
                else
                    tiles->RemoveClass(u8"checked");
            }
            if (auto* list = Find<gui::Button>(u8"assets-list"))
            {
                if (m_tiles)
                    list->RemoveClass(u8"checked");
                else
                    list->AddClass(u8"checked");
            }
            if (auto* clear = Find<gui::Button>(u8"assets-clear-search"))
                clear->SetVisible(!m_search.IsEmpty());
        }
        // The project's saved choice (tiles when it saved none).
        void LoadTiles()
        {
            bool tiles = true;
            if (foundation::settings::Settings* store = m_services.context != nullptr ? m_services.context->ProjectEditorSettings() : nullptr)
                if (const EditorAssetBrowserSettings* section = store->Find<EditorAssetBrowserSettings>())
                    tiles = section->gridMode;
            m_tiles = tiles;
            ShowLayout();
        }

        // The type filter: every type the project's assets have, by what each is called.
        void ShowTypeMenu()
        {
            auto* anchor = Find<gui::Button>(u8"assets-types");
            content::ContentDatabase* db = Sources();
            if (anchor == nullptr || db == nullptr)
                return;
            Array<String> types;
            Array<content::Group*> pending;
            pending.PushBack(db->RootGroup());
            while (!pending.IsEmpty())
            {
                content::Group* group = pending.Back();
                pending.PopBack();
                for (content::Instance* instance : group->Instances())
                {
                    bool known = false;
                    for (const String& type : types)
                        known = known || type.AsView() == instance->TypeName();
                    if (!known)
                        types.PushBack(String(instance->TypeName()));
                }
                for (content::Group* child : group->Groups())
                    pending.PushBack(child);
            }
            types.Sort([](const String& a, const String& b) { return KindOf(a.AsView()).AsView().Compare(KindOf(b.AsView()).AsView()) < 0; });
            m_menu = MakeRef<gui::Menu>(m_allocator);
            AssetsView* self = this;
            gui::MenuItem* every = m_menu->AddItem(u8"Every type", [self]() { self->SetTypeFilter(StringView{}); });
            every->SetCheckable(true);
            every->SetCheckedSilently(m_type.IsEmpty());
            m_menu->AddSeparator();
            for (const String& type : types)
            {
                gui::MenuItem* item = m_menu->AddItem(KindOf(type.AsView()).AsView(), [self, type]() { self->SetTypeFilter(type.AsView()); });
                item->SetCheckable(true);
                item->SetCheckedSilently(m_type == type);
            }
            m_menu->OpenBelow(*anchor, *anchor);
        }

        // === The verbs: renames from the models' edits, the menus, the confirmations ===
        bool RenameRow(usize row, StringView name)
        {
            if (row >= m_rows.Size() || m_services.operations == nullptr)
                return false;
            if (content::Group* group = m_rows[row].group)
                return RenameGroup(*group, name);
            const String refused = m_services.operations->Rename(m_rows[row].instance->Id(), name);
            if (!refused.IsEmpty())
                m_services.context->Notify(editor::NoticeKind::Warning, refused.AsView());
            return refused.IsEmpty();
        }
        bool RenameGroup(content::Group& group, StringView name)
        {
            if (m_services.operations == nullptr)
                return false;
            const String refused = m_services.operations->RenameGroup(group, name);
            if (!refused.IsEmpty())
                m_services.context->Notify(editor::NoticeKind::Warning, refused.AsView());
            return refused.IsEmpty();
        }
        // Assets a drag carries, moving into `group`: whether they can, and when not why.
        [[nodiscard]] bool CanTake(const gui::DragPayload& payload, const content::Group& group, String& why) const
        {
            Array<AssetDrag> assets;
            if (m_services.operations == nullptr || !AssetDrag::ReadAll(payload, assets))
                return false;
            Array<Guid> ids;
            for (const AssetDrag& asset : assets)
                ids.PushBack(asset.Id);
            const bool can = m_services.operations->CanMove(ids.AsSpan(), group, why);
            if (can)
                why = Format(u8"Move into {}", group.Parent() == nullptr ? StringView(kTopGroup) : group.Name());
            return can;
        }
        bool Take(const gui::DragPayload& payload, content::Group& group)
        {
            String why;
            if (!CanTake(payload, group, why))
                return false;
            Array<AssetDrag> assets;
            (void)AssetDrag::ReadAll(payload, assets);
            Array<Guid> ids;
            for (const AssetDrag& asset : assets)
                ids.PushBack(asset.Id);
            m_services.operations->MoveTo(Move(ids), group);
            return true;
        }
        [[nodiscard]] content::Group* TreeGroupAt(const gui::ModelIndex& index) const
        {
            const i64 node = index.InternalId;
            return node >= 0 && static_cast<usize>(node) < m_treeGroups.Size() ? m_treeGroups[static_cast<usize>(node)] : nullptr;
        }

        void ShowMenuAt(gui::Node& owner, Float2 at, i32 row)
        {
            if (m_services.operations == nullptr || Sources() == nullptr)
                return;
            if (row >= 0 && static_cast<usize>(row) < m_rows.Size())
            {
                if (content::Group* group = m_rows[static_cast<usize>(row)].group)
                    ShowGroupMenu(owner, at, *group, false, row);
                else
                    ShowAssetMenu(owner, at, row);
            }
            else if (m_current != nullptr)
                ShowGroupMenu(owner, at, *m_current, true);
        }
        // An asset's: what acts on one (open, rename, duplicate, copy, pin, export) on it, what
        // acts on many (cook, rebuild, delete) on the selection holding it.
        void ShowAssetMenu(gui::Node& owner, Float2 at, i32 row)
        {
            content::Instance& instance = *m_rows[static_cast<usize>(row)].instance;
            const Guid id = instance.Id();
            Array<Guid> targets;
            for (content::Instance* selected : SelectedAssets())
                targets.PushBack(selected->Id());
            bool holds = false;
            for (const Guid& target : targets)
                holds = holds || target == id;
            if (!holds)
            {
                targets.Clear();
                targets.PushBack(id);
            }
            AssetsView* self = this;
            AssetOperations& ops = *m_services.operations;
            m_menu = MakeRef<gui::Menu>(m_allocator);
            (void)m_menu->AddItem(u8"Open", [self, row]() { self->OpenRow(row); });
            m_menu->AddSeparator();
            (void)m_menu->AddItem(u8"Rename", [self, row]() { self->BeginRename(row); })->SetShortcut(u8"F2");
            (void)m_menu->AddItem(u8"Duplicate", [&ops, id]() { (void)ops.Duplicate(id); });
            m_menu->AddSeparator();
            (void)m_menu->AddItem(u8"Copy GUID", [self, id]() { self->CopyText(AssetPickerKey(id).AsView(), u8"GUID"); });
            (void)m_menu->AddItem(u8"Copy Path", [self, path = instance.Path()]() { self->CopyText(path.AsView(), u8"path"); });
            const bool favourite = m_services.context->IsFavorite(id);
            (void)m_menu->AddItem(favourite ? StringView(u8"Unpin Favourite") : StringView(u8"Pin as Favourite"), [self, id]()
                                  {
                                      self->m_services.context->ToggleFavorite(id);
                                      self->m_model.Changed();
                                  });
            gui::MenuItem* always = m_menu->AddItem(u8"Always Export", [self, &ops, id]()
                                                    {
                                                        ops.ToggleAlwaysExport(id);
                                                        self->m_model.Changed(); // its mark
                                                    });
            always->SetCheckable(true);
            always->SetCheckedSilently(ops.IsAlwaysExported(id));
            m_menu->AddSeparator();
            (void)m_menu->AddItem(targets.Size() == 1 ? String(u8"Cook").AsView() : Format(u8"Cook {} Assets", targets.Size()).AsView(),
                                  [&ops, targets]() { ops.Cook(targets, false); });
            (void)m_menu->AddItem(targets.Size() == 1 ? String(u8"Rebuild").AsView() : Format(u8"Rebuild {} Assets", targets.Size()).AsView(),
                                  [&ops, targets]() { ops.Cook(targets, true); });
            m_menu->AddSeparator();
            gui::MenuItem* remove = m_menu->AddItem(targets.Size() == 1 ? String(u8"Delete...").AsView() : Format(u8"Delete {} Assets...", targets.Size()).AsView(),
                                                    [self, targets]() { self->ConfirmDelete(targets); });
            remove->SetShortcut(u8"Del");
            m_menu->Open(owner, at);
        }
        // A group's (a row's, or the one shown: then also what makes new things in it).
        void ShowGroupMenu(gui::Node& owner, Float2 at, content::Group& group, bool shown, i32 row = -1)
        {
            if (m_services.operations == nullptr || Sources() == nullptr)
                return; // no verbs to offer (the tree's menu asks here too)
            AssetsView* self = this;
            AssetOperations& ops = *m_services.operations;
            content::Group* target = &group;
            m_menu = MakeRef<gui::Menu>(m_allocator);
            if (shown)
            {
                AddNewMenu(*m_menu, group);
                (void)m_menu->AddItem(u8"New Group", [self, &ops, target]()
                                      {
                                          if (content::Group* made = ops.CreateGroup(*target))
                                              self->Navigate(*made);
                                      });
                if (m_services.import)
                    (void)m_menu->AddItem(u8"Import...", [self, target]() { self->m_services.import(*target); });
                m_menu->AddSeparator();
            }
            else
            {
                (void)m_menu->AddItem(u8"Open", [self, target]() { self->Navigate(*target); });
                m_menu->AddSeparator();
            }
            const bool top = group.Parent() == nullptr;
            if (!top)
            {
                (void)m_menu->AddItem(u8"Rename", [self, target, row]() { self->BeginRenameGroup(*target, row); })->SetShortcut(u8"F2");
                gui::MenuItem* always = m_menu->AddItem(u8"Always Export Its Contents", [self, &ops, target]()
                                                        {
                                                            ops.ToggleAlwaysExport(*target);
                                                            self->m_model.Changed(); // its mark
                                                        });
                always->SetCheckable(true);
                always->SetCheckedSilently(ops.IsAlwaysExported(group));
                m_menu->AddSeparator();
            }
            (void)m_menu->AddItem(top ? StringView(u8"Cook All") : StringView(u8"Cook Group"), [&ops, target, top]()
                                  {
                                      if (top)
                                          ops.CookAll(false);
                                      else
                                          ops.CookGroup(*target, false);
                                  });
            (void)m_menu->AddItem(top ? StringView(u8"Rebuild All") : StringView(u8"Rebuild Group"), [&ops, target, top]()
                                  {
                                      if (top)
                                          ops.CookAll(true);
                                      else
                                          ops.CookGroup(*target, true);
                                  });
            if (!top)
            {
                m_menu->AddSeparator();
                (void)m_menu->AddItem(u8"Delete Group...", [self, target]() { self->ConfirmDeleteGroup(*target); });
            }
            m_menu->Open(owner, at);
        }
        // New: every creator, those of no category first, then a submenu per category.
        void AddNewMenu(gui::Menu& menu, content::Group& group)
        {
            if (!m_services.create || m_services.context == nullptr)
                return;
            gui::Menu* create = menu.AddSubMenu(u8"New");
            AssetsView* self = this;
            content::Group* into = &group;
            Array<StringView> categories;
            for (const pipeline::AssetCreator& creator : m_services.context->Creators().All())
            {
                if (creator.category.IsEmpty())
                {
                    const pipeline::AssetCreator* entry = &creator;
                    (void)create->AddItem(creator.label.AsView(), [self, entry, into]() { self->m_services.create(*entry, into); });
                    continue;
                }
                bool seen = false;
                for (StringView category : categories)
                    seen = seen || category == creator.category.AsView();
                if (!seen)
                    categories.PushBack(creator.category.AsView());
            }
            categories.Sort([](StringView a, StringView b) { return a.Compare(b) < 0; });
            if (!categories.IsEmpty())
                create->AddSeparator();
            for (StringView category : categories)
            {
                gui::Menu* sub = create->AddSubMenu(category);
                for (const pipeline::AssetCreator& creator : m_services.context->Creators().All())
                    if (creator.category.AsView() == category)
                    {
                        const pipeline::AssetCreator* entry = &creator;
                        (void)sub->AddItem(creator.label.AsView(), [self, entry, into]() { self->m_services.create(*entry, into); });
                    }
            }
        }
        void BeginRename(i32 row)
        {
            if (m_list != nullptr && row >= 0)
            {
                m_list->SetSelectedItem(row);
                (void)m_list->BeginEdit(row);
            }
        }
        // A group's row in place, else its node in the tree.
        void BeginRenameGroup(content::Group& group, i32 row)
        {
            if (row >= 0)
            {
                BeginRename(row);
                return;
            }
            if (m_tree == nullptr)
                return;
            for (usize node = 0; node < m_treeGroups.Size(); ++node)
                if (m_treeGroups[node] == &group)
                    for (i32 item = 0; item < static_cast<i32>(m_tree->GetItemCount()); ++item)
                        if (m_tree->GetItemIndex(item).InternalId == static_cast<i64>(node))
                            (void)m_tree->BeginEdit(item);
        }
        void CopyText(StringView text, StringView what)
        {
            gui::EventDispatcher* dispatcher = m_content ? m_content->GetEventDispatcher() : nullptr;
            (void)m_services.context->CopyText(dispatcher != nullptr ? dispatcher->GetClipboard() : nullptr, text, what);
        }
        [[nodiscard]] static String AssetPickerKey(const Guid& id)
        {
            utf8char chars[37];
            id.ToChars(chars);
            return String(StringView(chars, 36));
        }
        // Asks first, naming what goes and what follows; Delete deletes.
        void ConfirmDelete(Array<Guid> ids)
        {
            if (ids.IsEmpty() || m_services.operations == nullptr)
                return;
            const String names = m_services.operations->Names(ids.AsSpan());
            Confirm(ids.Size() == 1 ? String(u8"Delete Asset") : Format(u8"Delete {} Assets", ids.Size()),
                    Format(u8"Delete {}? {} source {} and cooked {} go, and {} open pages close.", names, ids.Size() == 1 ? StringView(u8"Its") : StringView(u8"Their"),
                           ids.Size() == 1 ? StringView(u8"file") : StringView(u8"files"), ids.Size() == 1 ? StringView(u8"product") : StringView(u8"products"),
                           ids.Size() == 1 ? StringView(u8"its") : StringView(u8"their")),
                    [ops = m_services.operations, ids = Move(ids)]() mutable { ops->Delete(Move(ids)); });
        }
        void ConfirmDeleteGroup(content::Group& group)
        {
            if (group.Parent() == nullptr || m_services.operations == nullptr)
                return;
            const usize count = AssetOperations::CountAssets(group);
            const String message = count == 0 ? Format(u8"Delete the empty group {}?", group.Name())
                                   : count == 1 ? Format(u8"Delete the group {} and the asset in it? Its source file and cooked product go, and its open page closes.", group.Name())
                                                : Format(u8"Delete the group {} and the {} assets in it? Their source files and cooked products go, and their open pages close.",
                                                         group.Name(), count);
            AssetsView* self = this;
            content::Group* target = &group;
            Confirm(String(u8"Delete Group"), message,
                    [self, target]() { self->m_services.operations->DeleteGroup(*target, [self](content::Group& next) { self->Navigate(next); }); });
        }
        void Confirm(const String& title, const String& message, Function<void()> yes)
        {
            gui::Node* root = m_content ? m_content->GetRootNode() : nullptr;
            if (root == nullptr)
                return;
            auto box = MakeRef<gui::MessageBox>(m_allocator);
            box->Configure(title.AsView(), message.AsView(), gui::MessageBox::Buttons::Ok);
            box->ClearButtons();
            (void)box->AddButton(u8"Cancel", u8"cancel", gui::DialogRole::Cancel);
            (void)box->AddButton(u8"Delete", u8"delete", gui::DialogRole::Destructive);
            gui::MessageBox* raw = box.Get();
            (void)box->AddEventListener(gui::EventType::Changed, [raw, yes = Move(yes)](const gui::Event&)
                                        {
                                            if (raw->GetResultName().AsView() == StringView(u8"delete") && yes)
                                                yes();
                                        });
            box->OpenModal(*root);
            m_confirm = box;
        }

        // A drag of the selection carries its assets (a group's row carries none).
        void DragPayloadFor(gui::DragPayload& payload, Span<const gui::ItemId> ids)
        {
            Array<AssetDrag> assets;
            for (const gui::ItemId id : ids)
                for (const Row& row : m_rows)
                    if (row.instance != nullptr && Model::IdOfRow(row) == id)
                        assets.PushBack(AssetDrag{row.instance->Id(), String(row.instance->TypeName()), String(row.instance->Name())});
            if (!assets.IsEmpty())
                payload = AssetDrag::Payload(assets.AsSpan());
        }

        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_content ? Cast<T>(FindIn(*m_content, id)) : nullptr;
        }
        static gui::Node* FindIn(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        static constexpr f32 kTileWidth = 96.0f;
        static constexpr f32 kTileHeight = 92.0f;

        IAllocator& m_allocator;
        Services m_services;
        RefPtr<gui::Node> m_content;
        gui::TreeView* m_tree = nullptr;
        gui::ListView* m_list = nullptr;
        gui::BreadcrumbBar* m_crumbs = nullptr;
        RefPtr<gui::Menu> m_menu;
        RefPtr<gui::MessageBox> m_confirm;
        u64 m_pictureListener = 0; // the context's thumbnail listener // the last confirmation asked (a test answers it)
        content::ContentDatabase* m_db = nullptr; // the database it shows (the project's)
        content::Group* m_current = nullptr;      // the group it shows
        u64 m_dbRevision = 0;
        u64 m_cookRevision = 0;
        String m_search;
        String m_type;
        bool m_tiles = true;
        bool m_building = false; // the tree is being filled: its selection is no navigation
        Array<Row> m_rows;
        Array<content::Group*> m_treeGroups; // by the tree's node id
        GroupTreeModel m_groupModel{*this};
        Model m_model;
    };

    RTTI_DEFINE_OBJECT(AssetCell, "rtti::editor::editor::app")
}
