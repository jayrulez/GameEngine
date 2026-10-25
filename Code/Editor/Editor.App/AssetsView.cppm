// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :assets_view partition
//
// The Assets panel (Documentation/Specs/editor-gui.md, 5.2, layer 7), over its markup
// (Data/Editor/Assets.gml) as the mock draws it: the project's source content, the content database
// the truth. The group tree on the left (folders, Content at the top); the toolbar's breadcrumb
// where it is; the group's subgroups then its assets on the right, as tiles or a list (kept per
// project), each by its type's glyph (its category's tint), its name, what it is, its cook badge
// and a star when a favourite. The search looks in every group, by name or a guid's start, each
// match saying where it is; the type filter keeps one type. With nothing to show it says what is
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

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace content = foundation::content;

    /// One asset or group as the browser draws it: a tile's picture over its name, or a list row's
    /// glyph, name, what it is, badge and star. Its class (`tile` or `row`) says which.
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
            bool Group = false;
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
            m_star = Part<gui::Image>(tile ? *m_thumb : static_cast<gui::Node&>(*this), u8"badge-icon");
            m_star->AddClass(u8"star");
            m_star->SetSource(String(u8"Editor/Icons/star.svg"));
            m_star->SetTooltip(u8"Favourite");
        }
        void Show(const Shown& shown)
        {
            if (!m_glyphClass.IsEmpty())
                m_glyph->RemoveClass(m_glyphClass.AsView());
            m_glyphClass = shown.GlyphClass;
            if (!m_glyphClass.IsEmpty())
                m_glyph->AddClass(m_glyphClass.AsView());
            m_glyph->SetSource(shown.Glyph);
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
            }
            else
            {
                m_badge->SetVisible(!shown.Badge.IsEmpty());
                m_star->SetVisible(shown.Favourite);
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
        [[nodiscard]] bool ShowsStar() const { return m_star->IsDisplayed() && m_star->IsVisible(); }
        [[nodiscard]] bool IsTile() const noexcept { return m_tile; }
        [[nodiscard]] gui::Label* NameLabel() const noexcept { return m_name.Get(); }
        [[nodiscard]] gui::Label* KindLabel() const noexcept { return m_kind.Get(); }

    private:
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
        RefPtr<gui::Panel> m_text; // a row's name and kind
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

        /// What it works over.
        struct Services
        {
            editor::EditorContext* context = nullptr;   // the project, favourites, the project's editor store
            editor::EditorCookService* cook = nullptr;  // the badges; null: none
            editor::EditorIcons* icons = nullptr;       // a type's glyph; null: generic ones
            Function<void(content::Instance&)> open;    // an asset opened (its page)
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
            }
            m_crumbs = Find<gui::BreadcrumbBar>(u8"assets-crumbs");
            if (m_crumbs != nullptr)
                m_crumbs->SetOnNavigate([self](const gui::BreadcrumbItem& item)
                                        {
                                            if (content::Group* group = self->GroupAt(item.Id.AsView()))
                                                self->Navigate(*group);
                                        });
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
        }
        AssetsView(const AssetsView&) = delete;
        AssetsView& operator=(const AssetsView&) = delete;
        ~AssetsView()
        {
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
        /// Shows `group`'s content (the search cleared), its tree row selected, the breadcrumb
        /// following.
        void Navigate(content::Group& group)
        {
            if (!Contains(&group))
                return;
            m_current = &group;
            if (!m_search.IsEmpty())
            {
                m_search = String{};
                if (auto* search = Find<gui::TextField>(u8"assets-search"))
                    search->SetText(StringView{});
            }
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
        /// The type filter's menu while it is open, for a test.
        [[nodiscard]] gui::Menu* TypeMenu() const noexcept { return m_menu.Get(); }

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
            void Reset() { NotifyReset(); }
            void Changed()
            {
                if (!m_view.m_rows.IsEmpty())
                    NotifyRowsChanged(gui::ModelIndex{}, 0, static_cast<i32>(m_view.m_rows.Size()));
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
        // What a type is called to the user: its displayName, else its name.
        [[nodiscard]] static String KindOf(StringView typeName)
        {
            for (const TypeInfo* type : GlobalTypeRegistry().All())
                if (type != nullptr && type->name != nullptr && StringView(reinterpret_cast<const utf8char*>(type->name)) == typeName)
                    return String(TypeAttrString(*type, "displayName", typeName));
            return String(typeName);
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
                    CollectMatches(*db->RootGroup());
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
                title = Format(u8"Nothing matches {}", m_search);
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
        gui::TreeModel m_groupModel;
        Model m_model;
    };

    RTTI_DEFINE_OBJECT(AssetCell, "rtti::editor::editor::app")
}
