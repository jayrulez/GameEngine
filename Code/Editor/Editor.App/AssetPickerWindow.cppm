// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_picker_window partition
//
// An asset slot's pick, shaped as the assets browser (Documentation/Specs/editor-gui.md, ruling 24;
// the mock's asset-picker.gml): the project's groups a tree at its side (Favourites and All first),
// what the chosen group holds of the slot's types beside it as tiles or a list (the last one chosen
// kept with the project), the current value lit, the search over every group while it has text (by
// name or a guid's start, as the browser's; each match saying where it lives). Enter or a double
// click picks; Esc or its close keeps the value (a slot's own clear sets nothing, so a pick can
// never clear it by a slip). A row's menu pins it as a favourite, or unpins it; a whole guid of an
// asset the slot does not take says so. Its
// cells are the browser's (AssetCell); it opens in the window's middle, as every picker does.
module;
#include "Core/Prelude.h"

export module editor.app:asset_picker_window;

import foundation.core;
import foundation.content;
import foundation.settings;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import :layout;      // EditorAssetBrowserSettings
import :assets_view; // AssetCell
import :asset_filter;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace content = foundation::content;

    /// What an asset slot asks the picker for.
    struct AssetPickRequest
    {
        Array<String> types;                // the asset types it takes, by name; empty: any
        Guid current;                       // its value now (nil: none)
        Function<void(const Guid&)> chosen; // a pick (nil: None)
    };

    class AssetPickerWindow
    {
    public:
        /// Its markup, under the data root.
        static constexpr const char8_t* kPath = u8"Editor/AssetPicker.gml";

        /// What it works over.
        struct Services
        {
            editor::EditorContext* context = nullptr; // the project, favourites, the project's editor store
            editor::EditorIcons* icons = nullptr;     // a type's glyph; null: generic ones
            /// An asset's picture (the thumbnails'), null while there is none; unset: glyphs only.
            Function<RefPtr<gui::Drawable>(const Guid&)> picture;
        };

        /// The picker in `content` (the markup's `asset-picker-search`, `asset-picker-tiles`,
        /// `asset-picker-list`, `asset-picker-groups`, `asset-picker-where`, `asset-picker-items`,
        /// `asset-picker-state`, `asset-picker-hint` and `asset-picker-close`, by id).
        AssetPickerWindow(IAllocator& allocator, Services services, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_services(Move(services)), m_content(Move(content)), m_sources(allocator), m_rows(allocator), m_model(*this)
        {
            if (!m_content)
                return;
            m_search = Find<gui::TextField>(u8"asset-picker-search");
            m_tree = Find<gui::TreeView>(u8"asset-picker-groups");
            m_list = Find<gui::ListView>(u8"asset-picker-items");
            m_where = Find<gui::Label>(u8"asset-picker-where");
            m_state = Find<gui::Label>(u8"asset-picker-state");
            m_hint = Find<gui::Label>(u8"asset-picker-hint");
            AssetPickerWindow* self = this;
            if (m_search != nullptr)
                m_search->SetOnTextChanging([self](StringView text) { self->Search(text); });
            if (m_tree != nullptr)
            {
                m_tree->SetModel(&m_groups);
                m_tree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                              {
                                                  if (self->m_building || !index.IsValid())
                                                      return;
                                                  self->m_source = static_cast<usize>(index.InternalId);
                                                  self->BuildRows();
                                              });
            }
            if (m_list != nullptr)
            {
                gui::RowFactory rows;
                rows.TypeOf = [self](const gui::ModelIndex&) { return self->m_tiles ? 0u : 1u; };
                rows.Create = [self](u32 type) -> RefPtr<gui::Node> { return MakeRef<AssetCell>(self->m_allocator, type == 0u); };
                rows.Bind = [self](gui::Node& cell, const gui::ModelIndex& index) { self->BindCell(cell, index.Row); };
                m_list->SetRowFactory(Move(rows));
                m_list->SetModel(&m_model);
                m_list->SetOnActivated([self](gui::ModelIndex index) { self->PickRow(index.Row); });
                (void)m_list->AddEventListener(gui::EventType::ContextMenu, [self](const gui::Event& e)
                                               {
                                                   const Float2 at = static_cast<const gui::MouseEvent&>(e).Position;
                                                   self->ShowRowMenu(self->m_list->ItemAt(at), at);
                                                   e.Handled = true;
                                               });
            }
            if (auto* tiles = Find<gui::Button>(u8"asset-picker-tiles"))
                tiles->SetOnClick([self]() { self->SetTiles(true); });
            if (auto* list = Find<gui::Button>(u8"asset-picker-list"))
                list->SetOnClick([self]() { self->SetTiles(false); });
            if (auto* close = Find<gui::Button>(u8"asset-picker-close"))
                close->SetOnClick([self]() { self->Close(); });
            // The arrows and Enter before the search takes them; in tiles Up and Down cross a row.
            (void)m_content->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                              {
                                                  const auto& key = static_cast<const gui::KeyEvent&>(e);
                                                  const auto code = static_cast<gui::KeyCode>(key.KeyCode);
                                                  if (key.Modifiers != 0)
                                                      return;
                                                  const i32 across = self->m_tiles && self->m_list != nullptr ? Max(1, self->m_list->GetColumnsAcross()) : 1;
                                                  if (code == gui::KeyCode::Down || code == gui::KeyCode::Up)
                                                  {
                                                      self->Step(code == gui::KeyCode::Down ? across : -across);
                                                      e.Handled = true;
                                                  }
                                                  else if (self->m_tiles && (code == gui::KeyCode::Right || code == gui::KeyCode::Left))
                                                  {
                                                      self->Step(code == gui::KeyCode::Right ? 1 : -1);
                                                      e.Handled = true;
                                                  }
                                                  else if (code == gui::KeyCode::Return || code == gui::KeyCode::KeypadEnter)
                                                  {
                                                      self->PickLit();
                                                      e.Handled = true;
                                                  }
                                              },
                                              true);
            if (m_services.context != nullptr)
                m_thumbnails = m_services.context->AddThumbnailListener([self](const Guid&)
                                                                        {
                                                                            if (self->IsOpen())
                                                                                self->m_model.Changed();
                                                                        });
        }
        AssetPickerWindow(const AssetPickerWindow&) = delete;
        AssetPickerWindow& operator=(const AssetPickerWindow&) = delete;
        ~AssetPickerWindow()
        {
            Close();
            if (m_services.context != nullptr)
                m_services.context->RemoveThumbnailListener(m_thumbnails);
            if (m_list != nullptr)
                m_list->SetModel(nullptr);
            if (m_tree != nullptr)
                m_tree->SetModel(nullptr);
        }

        /// Opens it over `root` for `request`, in the window's middle: the current value's group
        /// chosen (else All) and its row lit, the search empty and focused, tiles or a list as it
        /// was left. False with no frame, no project, a root in no GUI, or one open already.
        bool Open(gui::SceneRoot& root, AssetPickRequest request)
        {
            gui::EventDispatcher* dispatcher = root.GetEventDispatcher();
            content::ContentDatabase* db = Sources();
            if (!m_content || dispatcher == nullptr || IsOpen() || db == nullptr)
                return false;
            m_root = &root;
            m_request = Move(request);
            m_query.Clear();
            m_tiles = StoredTiles();
            if (m_search != nullptr)
            {
                m_search->SetPlaceholder(Format(u8"Search {}", Lowered(Noun())).AsView());
                m_search->SetText(StringView{});
            }
            BuildTree();
            content::Instance* current = m_request.current.IsNil() ? nullptr : db->GetInstance(m_request.current);
            m_source = kAll;
            if (current != nullptr)
                for (usize i = 0; i < m_sources.Size(); ++i)
                    if (m_sources[i].group == &current->OwningGroup())
                        m_source = i;
            SelectSource(m_source);
            ShowLayout();
            BuildRows();
            if (m_hint != nullptr)
                m_hint->SetText(current != nullptr ? Format(u8"Enter picks, Esc keeps {}", current->Name()).AsView() : StringView(u8"Enter picks, Esc closes"));
            gui::PopupOptions options;
            options.Kind = gui::PopupKind::Auto;
            options.Modal = true;
            options.TakesFocus = true;
            options.FitContent = true;
            options.Centered = true;
            dispatcher->OpenPopup(root, *m_content, Move(options));
            if (m_search != nullptr)
                m_search->RequestFocus();
            return true;
        }
        void Close()
        {
            if (IsOpen())
                (void)m_root->GetEventDispatcher()->ClosePopup(*m_content);
        }
        [[nodiscard]] bool IsOpen() const
        {
            return m_root != nullptr && m_content && m_root->GetEventDispatcher() != nullptr && m_root->GetEventDispatcher()->IsPopupOpen(*m_content);
        }

        /// Tiles or a list, kept with the project for the next pick.
        void SetTiles(bool tiles)
        {
            m_tiles = tiles;
            if (foundation::settings::Settings* store = m_services.context != nullptr ? m_services.context->ProjectEditorSettings() : nullptr)
            {
                store->Section<EditorAssetBrowserSettings>().pickerGridMode = tiles;
                store->MarkChanged<EditorAssetBrowserSettings>();
                m_services.context->RequestProjectEditorSettingsSave();
            }
            ShowLayout();
        }
        /// Searches every group for `text` (as typing it); empty: the chosen group again.
        void Search(StringView text)
        {
            if (m_query.AsView() == text)
                return;
            m_query = String(text);
            if (m_search != nullptr && m_search->GetText() != text)
                m_search->SetText(text);
            BuildRows();
        }
        /// Chooses the group whose tree row reads `name` (as a click on it); false when none does.
        bool ChooseSource(StringView name)
        {
            for (usize i = 0; i < m_sources.Size(); ++i)
                if (m_sources[i].name.AsView() == name)
                {
                    m_source = i;
                    SelectSource(i);
                    BuildRows();
                    return true;
                }
            return false;
        }
        /// A row's menu at `at` (the list's space): Pin as Favourite, or Unpin Favourite. No row, or
        /// no context: none.
        void ShowRowMenu(i32 row, Float2 at)
        {
            if (m_list == nullptr || m_services.context == nullptr || row < 0 || static_cast<usize>(row) >= m_rows.Size())
                return;
            const Guid id = m_rows[static_cast<usize>(row)]->Id();
            AssetPickerWindow* self = this;
            m_menu = MakeRef<gui::Menu>(m_allocator);
            (void)m_menu->AddItem(m_services.context->IsFavorite(id) ? StringView(u8"Unpin Favourite") : StringView(u8"Pin as Favourite"), [self, id]()
                                  {
                                      self->m_services.context->ToggleFavorite(id);
                                      if (self->m_source == kFavourites && self->m_query.IsEmpty())
                                          self->BuildRows(); // it joins or leaves the rows
                                      else
                                          self->m_model.Changed(); // its star
                                  });
            m_menu->Open(*m_list, at);
        }
        /// The row menu, while it is open.
        [[nodiscard]] gui::Menu* RowMenu() const noexcept { return m_menu && m_menu->IsOpen() ? m_menu.Get() : nullptr; }
        /// Picks the lit row; with none lit, nothing.
        void PickLit() { PickRow(m_list != nullptr ? m_list->GetSelectedItem() : -1); }
        void Step(i32 by)
        {
            if (m_list == nullptr || m_rows.IsEmpty())
                return;
            const i32 lit = m_list->GetSelectedItem();
            Light(lit < 0 ? 0 : Clamp(lit + by, 0, static_cast<i32>(m_rows.Size()) - 1));
        }

        // === What it shows (for tests) ===
        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }
        [[nodiscard]] bool ShowsTiles() const noexcept { return m_tiles; }
        [[nodiscard]] gui::ListView* List() const noexcept { return m_list; }
        [[nodiscard]] gui::Button* CloseButton() const { return Find<gui::Button>(u8"asset-picker-close"); }
        /// The tree's rows by name, in order ("Favourites | All Materials | Content | Props").
        [[nodiscard]] String Groups() const
        {
            String out;
            for (const Source& source : m_sources)
            {
                if (!out.IsEmpty())
                    out += u8" | ";
                out += source.name.AsView();
            }
            return out;
        }
        /// The rows shown by name, in order ("Wood | Wool").
        [[nodiscard]] String Rows() const
        {
            String out;
            for (const content::Instance* row : m_rows)
            {
                if (!out.IsEmpty())
                    out += u8" | ";
                out += row->Name();
            }
            return out;
        }
        [[nodiscard]] StringView Where() const { return m_where != nullptr ? m_where->GetText() : StringView{}; }
        [[nodiscard]] StringView Hint() const { return m_hint != nullptr ? m_hint->GetText() : StringView{}; }
        /// What it says with no rows to show; empty while it shows rows.
        [[nodiscard]] StringView State() const { return m_state != nullptr && m_state->IsDisplayed() ? m_state->GetText() : StringView{}; }
        [[nodiscard]] StringView LitName() const
        {
            const i32 lit = m_list != nullptr ? m_list->GetSelectedItem() : -1;
            return lit >= 0 && static_cast<usize>(lit) < m_rows.Size() ? m_rows[static_cast<usize>(lit)]->Name() : StringView{};
        }

    private:
        static constexpr usize kFavourites = 0;
        static constexpr usize kAll = 1;
        static constexpr f32 kTileWidth = 96.0f;
        static constexpr f32 kTileHeight = 92.0f;

        // A row of the tree, by its node: Favourites, All, or a group.
        struct Source
        {
            String name;
            content::Group* group = nullptr; // null: Favourites or All
        };

        // The rows as the list's model, an asset's id its guid's.
        class Model final : public gui::IModel
        {
        public:
            explicit Model(AssetPickerWindow& owner) : m_owner(owner) {}
            [[nodiscard]] usize RowCount(const gui::ModelIndex& parent = {}) const override { return parent.IsValid() ? 0 : m_owner.m_rows.Size(); }
            [[nodiscard]] usize ColumnCount() const override { return 1; }
            [[nodiscard]] gui::Variant Data(const gui::ModelIndex& index, gui::ModelRole role = gui::ModelRole::Display) const override
            {
                return role == gui::ModelRole::Display && index.Row >= 0 && static_cast<usize>(index.Row) < m_owner.m_rows.Size()
                           ? gui::Variant(m_owner.m_rows[static_cast<usize>(index.Row)]->Name())
                           : gui::Variant{};
            }
            [[nodiscard]] gui::ItemId IdOf(const gui::ModelIndex& index) const override
            {
                if (index.Row < 0 || static_cast<usize>(index.Row) >= m_owner.m_rows.Size())
                    return gui::kNoItem;
                return IdOfRow(*m_owner.m_rows[static_cast<usize>(index.Row)]);
            }
            [[nodiscard]] gui::ModelIndex IndexOfId(gui::ItemId id) const override
            {
                for (usize row = 0; row < m_owner.m_rows.Size(); ++row)
                    if (IdOfRow(*m_owner.m_rows[row]) == id)
                        return gui::ModelIndex{static_cast<i32>(row), 0};
                return {};
            }
            [[nodiscard]] static gui::ItemId IdOfRow(const content::Instance& instance)
            {
                const Guid& id = instance.Id();
                return (id.high ^ (id.low * 31u)) >> 1;
            }
            void Reset() { NotifyReset(); }
            void Changed()
            {
                if (!m_owner.m_rows.IsEmpty())
                    NotifyRowsChanged(gui::ModelIndex{}, 0, static_cast<i32>(m_owner.m_rows.Size()));
            }

        private:
            AssetPickerWindow& m_owner;
        };

        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            Array<gui::Node*> pending;
            pending.PushBack(m_content.Get());
            while (!pending.IsEmpty())
            {
                gui::Node* node = pending.Back();
                pending.PopBack();
                if (auto* widget = Cast<gui::UIWidget>(node); widget != nullptr && widget->GetId() == id)
                    return Cast<T>(node);
                for (usize i = 0; i < node->ChildCount(); ++i)
                    pending.PushBack(node->GetChildAt(i));
            }
            return nullptr;
        }
        [[nodiscard]] content::ContentDatabase* Sources() const
        {
            editor::EditorProject* project = m_services.context != nullptr ? m_services.context->Project() : nullptr;
            return project != nullptr ? &project->SourceDb() : nullptr;
        }
        [[nodiscard]] bool StoredTiles() const
        {
            const foundation::settings::Settings* store = m_services.context != nullptr ? m_services.context->ProjectEditorSettings() : nullptr;
            const EditorAssetBrowserSettings* section = store != nullptr ? store->Find<EditorAssetBrowserSettings>() : nullptr;
            return section != nullptr && section->pickerGridMode;
        }
        [[nodiscard]] bool Takes(const content::Instance& instance) const
        {
            if (m_request.types.IsEmpty())
                return true;
            for (const String& type : m_request.types)
                if (type.AsView() == instance.TypeName())
                    return true;
            return false;
        }

        // What the slot takes, plural, by its type's display name ("Materials", "Meshes"); more
        // than one type, or any, "Assets".
        [[nodiscard]] String Noun() const
        {
            if (m_request.types.Size() != 1)
                return String(u8"Assets");
            const String kind = AssetsView::KindOf(m_request.types[0].AsView());
            const StringView name = kind.AsView();
            if (name.EndsWith(u8"s") || name.EndsWith(u8"x") || name.EndsWith(u8"sh") || name.EndsWith(u8"ch"))
                return Format(u8"{}es", name);
            const utf8char before = name.Size() >= 2 ? name[name.Size() - 2] : u8'a';
            const bool vowel = before == u8'a' || before == u8'e' || before == u8'i' || before == u8'o' || before == u8'u';
            if (name.EndsWith(u8"y") && !vowel) // "Body", "Bodies"; "Key", "Keys"
                return Format(u8"{}ies", name.SubStr(0, name.Size() - 1));
            return Format(u8"{}s", name);
        }
        [[nodiscard]] static String Lowered(StringView text)
        {
            String out(text);
            for (usize i = 0; i < out.Size(); ++i)
                if (out[i] >= u8'A' && out[i] <= u8'Z')
                    out[i] = static_cast<utf8char>(out[i] + (u8'a' - u8'A'));
            return out;
        }
        [[nodiscard]] static String ShownPath(const content::Group& group)
        {
            return group.Parent() == nullptr ? String(AssetsView::kTopGroup) : Format(u8"{}/{}", AssetsView::kTopGroup, group.Path());
        }

        // Favourites, All, then the project's groups (the top one Content), every one open.
        void BuildTree()
        {
            if (m_tree == nullptr)
                return;
            m_building = true;
            m_groups.Clear();
            m_sources.Clear();
            AddSource(m_groups.AddNode(gui::TreeModel::kRoot, u8"Favourites"), u8"Editor/Icons/star.svg", String(u8"Favourites"), nullptr);
            const String all = Format(u8"All {}", Noun());
            AddSource(m_groups.AddNode(gui::TreeModel::kRoot, all.AsView()), u8"Editor/Icons/layers.svg", all, nullptr);
            if (content::ContentDatabase* db = Sources())
                AddGroup(*db->RootGroup(), gui::TreeModel::kRoot);
            for (usize node = 0; node < m_sources.Size(); ++node)
                m_tree->SetIdExpanded(static_cast<gui::ItemId>(node), true);
            m_building = false;
        }
        void AddSource(i32 node, StringView icon, String name, content::Group* group)
        {
            m_groups.SetIcon(node, icon);
            while (m_sources.Size() <= static_cast<usize>(node))
                m_sources.PushBack(Source{});
            m_sources[static_cast<usize>(node)] = Source{Move(name), group};
        }
        void AddGroup(content::Group& group, i32 parent)
        {
            const StringView name = group.Parent() == nullptr ? AssetsView::kTopGroup : group.Name();
            const i32 node = m_groups.AddNode(parent, name);
            AddSource(node, u8"Editor/Icons/folder.svg", String(name), &group);
            for (content::Group* child : group.Groups())
                AddGroup(*child, node);
        }
        void SelectSource(usize source)
        {
            if (m_tree == nullptr)
                return;
            const bool building = m_building;
            m_building = true; // a selection from code is no choice
            for (i32 item = 0; item < static_cast<i32>(m_tree->GetItemCount()); ++item)
                if (m_tree->GetItemIndex(item).InternalId == static_cast<i64>(source))
                    m_tree->SetSelectedItem(item); // every group is open: each is an item
            m_building = building;
        }

        // What the chosen source holds of the slot's types, in the groups' order; a search's
        // matches from every group; then the count and the state.
        void BuildRows()
        {
            m_rows.Clear();
            if (content::ContentDatabase* db = Sources())
            {
                if (!m_query.IsEmpty() || m_source == kAll)
                    Collect(*db->RootGroup(), true);
                else if (m_source == kFavourites && m_services.context != nullptr)
                {
                    for (const Guid& id : m_services.context->Favorites())
                        if (content::Instance* instance = db->GetInstance(id); instance != nullptr && Takes(*instance))
                            m_rows.PushBack(instance);
                }
                else if (m_source < m_sources.Size() && m_sources[m_source].group != nullptr)
                    Collect(*m_sources[m_source].group, false);
            }
            m_model.Reset();
            // The current value lit when it is shown, else the first row.
            i32 lit = m_rows.IsEmpty() ? -1 : 0;
            for (usize i = 0; i < m_rows.Size(); ++i)
                if (m_rows[i]->Id() == m_request.current)
                    lit = static_cast<i32>(i);
            if (lit >= 0)
                Light(lit);
            if (m_where != nullptr)
            {
                const String count = Format(u8"{} {}", m_rows.Size(), Lowered(Noun()));
                const String where = !m_query.IsEmpty() ? Format(u8"In every group  ·  {}", count)
                                     : m_source < m_sources.Size() ? Format(u8"{}  ·  {}", m_sources[m_source].name, count)
                                                                   : count;
                m_where->SetText(where.AsView());
            }
            if (m_state != nullptr)
            {
                m_state->SetDisplayed(m_rows.IsEmpty());
                if (const content::Instance* other = WrongTypeNamed(); other != nullptr)
                    m_state->SetText(Format(u8"'{}' is a {}, which this slot does not take.", other->Name(), AssetsView::KindOf(other->TypeName())).AsView());
                else if (!m_query.IsEmpty())
                    m_state->SetText(Format(u8"No {} match \"{}\".", Lowered(Noun()), m_query).AsView());
                else if (m_source == kFavourites)
                    m_state->SetText(Format(u8"No favourite {}: star one in the Assets panel.", Lowered(Noun())).AsView());
                else
                    m_state->SetText(Format(u8"No {} here.", Lowered(Noun())).AsView());
            }
        }
        // The asset a search's whole guid names when the slot does not take its type; else null.
        [[nodiscard]] const content::Instance* WrongTypeNamed() const
        {
            Guid id;
            content::ContentDatabase* db = Sources();
            if (db == nullptr || !editor::FilterAsGuid(m_query.AsView(), id))
                return nullptr;
            const content::Instance* named = db->GetInstance(id);
            return named != nullptr && !Takes(*named) ? named : nullptr;
        }
        void Collect(content::Group& group, bool deep)
        {
            for (content::Instance* instance : group.Instances())
                if (Takes(*instance) && AssetMatchesFilter(*instance, m_query.AsView()))
                    m_rows.PushBack(instance);
            if (deep)
                for (content::Group* child : group.Groups())
                    Collect(*child, true);
        }
        // Rows from more than one group say where each lives.
        [[nodiscard]] bool RowsSayWhere() const { return !m_query.IsEmpty() || m_source == kAll || m_source == kFavourites; }

        void BindCell(gui::Node& node, i32 index)
        {
            auto* cell = Cast<AssetCell>(&node);
            if (cell == nullptr || index < 0 || static_cast<usize>(index) >= m_rows.Size())
                return;
            const content::Instance& instance = *m_rows[static_cast<usize>(index)];
            AssetCell::Shown shown;
            shown.Name = String(instance.Name());
            shown.Kind = AssetsView::KindOf(instance.TypeName());
            if (RowsSayWhere())
                shown.Kind = Format(u8"{} in {}", shown.Kind, ShownPath(instance.OwningGroup()));
            const TypeInfo* type = GlobalTypeRegistry().FindByName(reinterpret_cast<const char*>(String(instance.TypeNamespace()).CStr()),
                                                                   reinterpret_cast<const char*>(String(instance.TypeName()).CStr()));
            if (type != nullptr && m_services.icons != nullptr)
            {
                const editor::TypeIcon icon = m_services.icons->For(*type, u8"Asset");
                shown.Glyph = icon.Source;
                shown.GlyphClass = icon.Class;
            }
            if (shown.Glyph.IsEmpty())
                shown.Glyph = String(u8"Editor/Icons/file.svg");
            shown.StateColumns = false; // a pick asks neither its cook nor its export
            shown.Favourite = m_services.context != nullptr && m_services.context->IsFavorite(instance.Id());
            if (m_services.picture)
                shown.Picture = m_services.picture(instance.Id()); // a tile's, and a row's small
            cell->Show(shown);
            // Hovered, the whole of where it lives (a row's is cut to fit) and what it is.
            cell->SetTooltip(Format(u8"{}/{} ({})", ShownPath(instance.OwningGroup()), instance.Name(), AssetsView::KindOf(instance.TypeName())).AsView());
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
            ShowChecked(u8"asset-picker-tiles", m_tiles);
            ShowChecked(u8"asset-picker-list", !m_tiles);
        }
        void ShowChecked(StringView id, bool checked)
        {
            if (auto* button = Find<gui::Button>(id))
            {
                if (checked)
                    button->AddClass(u8"checked");
                else
                    button->RemoveClass(u8"checked");
            }
        }

        void Light(i32 row)
        {
            if (m_list == nullptr || row < 0)
                return;
            m_list->SetSelectedItem(row);
            m_list->ScrollToItem(row);
        }
        void PickRow(i32 row)
        {
            if (row >= 0 && static_cast<usize>(row) < m_rows.Size())
                Pick(m_rows[static_cast<usize>(row)]->Id());
        }
        void Pick(const Guid& id)
        {
            Function<void(const Guid&)> chosen = Move(m_request.chosen);
            Close();
            if (chosen)
                chosen(id);
        }

        IAllocator& m_allocator;
        Services m_services;
        RefPtr<gui::Node> m_content;
        gui::SceneRoot* m_root = nullptr;
        gui::TextField* m_search = nullptr;
        gui::TreeView* m_tree = nullptr;
        gui::ListView* m_list = nullptr;
        gui::Label* m_where = nullptr;
        gui::Label* m_state = nullptr;
        gui::Label* m_hint = nullptr;
        gui::TreeModel m_groups;
        Array<Source> m_sources; // by the tree's node
        Array<content::Instance*> m_rows;
        Model m_model;
        RefPtr<gui::Menu> m_menu; // a row's
        AssetPickRequest m_request;
        String m_query;
        usize m_source = kAll;
        bool m_tiles = false;
        bool m_building = false;
        u64 m_thumbnails = 0;
    };
}
