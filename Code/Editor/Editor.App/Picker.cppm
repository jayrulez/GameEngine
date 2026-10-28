// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :picker partition
//
// The picker (Documentation/Specs/editor-gui.md, 5.2, layer 5; catalog 2.4): one for paths, groups
// and components (an asset slot's is the asset picker, AssetPickerWindow), as the kit draws it (Data/Editor/Picker.xml). A search over the rows, which sit under
// their sections in the order the owner gives them, favourites first in a section of their own; a
// row shows its glyph (its category's tint), its name, what it is, a star when a favourite, and a
// check on the current value. The current value is lit when it opens; Up and Down move the light
// past the section headings while the search keeps the keys, and Enter, a double click or Enter on
// the list picks. The foot says so ("Esc keeps" the current value), and a close at the search's end
// keeps it too: no button sets nothing, so a pick never clears a value by a slip. With nothing to
// pick it says what is missing and offers the owner's next step;
// with nothing matching, it says so and offers to clear the search. It opens as a modal popup in
// the window's middle.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:picker;

import foundation.core;
import foundation.gui;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// One thing a picker offers.
    struct PickerItem
    {
        String Key;        ///< what a pick answers (a guid's text, a path); unique in a request
        String Name;
        String Detail;     ///< what it is, dim after the name (a type, a folder): a few words
        String Description; ///< a sentence about it, the foot's while its row is lit (a component's)
        String Section;    ///< the heading it is listed under
        String Glyph;      ///< its icon's source
        String GlyphClass; ///< the class tinting it (a category's)
        bool Favourite = false;
        u32 Depth = 0;     ///< its level in a tree request, under the item before it one level up
    };

    /// What a picker is opened with.
    struct PickerRequest
    {
        String Placeholder;      ///< the search's ("Search materials")
        Array<PickerItem> Items; ///< in their order; each section where its first item is
        String Current;          ///< the current value's key: checked, and lit when it opens
        bool Tree = false;       ///< the items a tree (by Depth, in their order): a search keeps where each match is
        String EmptyTitle;       ///< with nothing to pick ("No materials yet")
        String EmptyText;
        String EmptyVerb;        ///< the next step it offers then ("Import..."), run after it closes
        Function<void()> OnEmptyVerb;
        Function<void(StringView key)> OnPicked;
    };

    /// One row: an item's glyph, name, detail, favourite star and current check.
    class PickerRow final : public gui::UIWidget
    {
        RTTI_OBJECT(PickerRow, gui::UIWidget)
    public:
        PickerRow()
        {
            AddClass(u8"asset-row");
            m_indent = Part<gui::Panel>(u8"picker-indent");
            m_glyph = Part<gui::Image>(u8"row-icon");
            m_name = Part<gui::Label>(u8"row-name");
            m_detail = Part<gui::Label>(u8"row-kind");
            m_star = Part<gui::Image>(u8"badge-icon");
            m_star->AddClass(u8"star");
            m_star->SetSource(String(u8"Editor/Icons/star.svg"));
            m_star->SetTooltip(u8"Favourite");
            m_check = Part<gui::Image>(u8"row-trail");
            m_check->SetSource(String(u8"Editor/Icons/check.svg"));
            m_check->SetTooltip(u8"The current value");
        }
        void Show(const PickerItem& item, bool current, bool context = false)
        {
            m_indentLevels = item.Depth;
            m_indent->SetDisplayed(item.Depth > 0);
            if (item.Depth > 0)
                m_indent->SetInlineStyle(u8"width", Format(u8"{}px", item.Depth * kIndent).AsView());
            if (context)
                AddClass(u8"context");
            else
                RemoveClass(u8"context");
            if (!m_glyphClass.IsEmpty())
                m_glyph->RemoveClass(m_glyphClass.AsView());
            m_glyphClass = item.GlyphClass;
            if (!m_glyphClass.IsEmpty())
                m_glyph->AddClass(m_glyphClass.AsView());
            m_glyph->SetSource(item.Glyph);
            m_glyph->SetDisplayed(!item.Glyph.IsEmpty());
            m_name->SetText(item.Name.AsView());
            m_detail->SetText(item.Detail.AsView());
            m_star->SetDisplayed(item.Favourite);
            m_check->SetDisplayed(current);
        }
        [[nodiscard]] StringView Name() const { return m_name->GetText(); }
        [[nodiscard]] bool ShowsCheck() const { return m_check->IsDisplayed(); }
        [[nodiscard]] bool ShowsStar() const { return m_star->IsDisplayed(); }
        /// How deep it is indented, in levels; whether it is shown only as where a match is.
        [[nodiscard]] u32 Indent() const { return m_indentLevels; }
        [[nodiscard]] bool IsContext() const { return HasClass(u8"context"); }

    private:
        static constexpr u32 kIndent = 16; // a level's indent, in pixels

        template <typename T>
        RefPtr<T> Part(StringView cls)
        {
            RefPtr<T> part = MakeRef<T>(MemoryAllocator());
            part->AddClass(cls);
            part->SetHitTestVisible(false);
            AddChild(part.Get());
            return part;
        }

        RefPtr<gui::Panel> m_indent;
        u32 m_indentLevels = 0;
        RefPtr<gui::Image> m_glyph;
        RefPtr<gui::Label> m_name;
        RefPtr<gui::Label> m_detail;
        RefPtr<gui::Image> m_star;
        RefPtr<gui::Image> m_check;
        String m_glyphClass;
    };

    class Picker
    {
    public:
        static constexpr const char8_t* kPath = u8"Editor/Picker.xml";
        static constexpr StringView kFavourites = u8"Favourites";

        /// The picker in `content` (the markup's: `picker-search`, `picker-list`, `picker-state` and
        /// its parts, `picker-hint`, `picker-close`, by id).
        Picker(IAllocator& allocator, RefPtr<gui::Node> content) : m_allocator(allocator), m_content(Move(content)), m_rows(allocator), m_model(*this)
        {
            if (!m_content)
                return;
            m_search = Find<gui::TextField>(u8"picker-search");
            m_list = Find<gui::ListView>(u8"picker-list");
            m_state = Find<gui::UIWidget>(u8"picker-state");
            m_hint = Find<gui::Label>(u8"picker-hint");
            Picker* self = this;
            if (m_search != nullptr)
                m_search->SetOnTextChanging([self](StringView text) { self->Filter(text); });
            if (m_list != nullptr)
            {
                gui::RowFactory rows;
                rows.TypeOf = [self](const gui::ModelIndex& index) { return self->IsHeader(index.Row) ? 1u : 0u; };
                rows.Create = [self](u32 type) { return self->MakeRow(type); };
                rows.Bind = [self](gui::Node& row, const gui::ModelIndex& index) { self->BindRow(row, index.Row); };
                m_list->SetRowFactory(Move(rows));
                m_list->SetEstimatedRowHeight(kRowEstimate);
                m_list->SetModel(&m_model);
                m_list->SetOnActivated([self](gui::ModelIndex index) { self->PickRow(index.Row); });
                m_list->SetOnSelectionChanged([self](gui::ModelIndex) { self->ShowHint(); });
            }
            if (auto* close = Find<gui::Button>(u8"picker-close"))
                close->SetOnClick([self]() { self->Close(); });
            if (auto* verb = Find<gui::Button>(u8"picker-state-verb"))
                verb->SetOnClick([self]() { self->StateVerb(); });
            // Up, Down and Enter before the search takes them.
            (void)m_content->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                              {
                                                  const auto& key = static_cast<const gui::KeyEvent&>(e);
                                                  const auto code = static_cast<gui::KeyCode>(key.KeyCode);
                                                  if (key.Modifiers != 0)
                                                      return;
                                                  if (code == gui::KeyCode::Down || code == gui::KeyCode::Up)
                                                  {
                                                      self->Step(code == gui::KeyCode::Down ? 1 : -1);
                                                      e.Handled = true;
                                                  }
                                                  else if (code == gui::KeyCode::Return || code == gui::KeyCode::KeypadEnter)
                                                  {
                                                      self->PickLit();
                                                      e.Handled = true;
                                                  }
                                              },
                                              true);
        }
        Picker(const Picker&) = delete;
        Picker& operator=(const Picker&) = delete;
        ~Picker()
        {
            Close();
            if (m_list != nullptr)
                m_list->SetModel(nullptr);
        }

        /// Opens it over `root` for `request`, in the window's middle whatever asked (every picker
        /// the same: the user, 2026-10-25); the search empty and focused, the current value lit.
        /// False with no frame, a root in no GUI, or one open already.
        bool Open(gui::SceneRoot& root, PickerRequest request)
        {
            gui::EventDispatcher* dispatcher = root.GetEventDispatcher();
            if (!m_content || dispatcher == nullptr || IsOpen())
                return false;
            m_root = &root;
            m_request = Move(request);
            if (m_search != nullptr)
            {
                m_search->SetPlaceholder(m_request.Placeholder.AsView());
                m_search->SetText(StringView{});
            }
            Filter(StringView{});
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

        /// Filters for `text`, as typing it in the search does.
        void SetQuery(StringView text)
        {
            if (m_search != nullptr)
                m_search->SetText(text);
            Filter(text);
        }
        /// The rows shown, headings included, and what each says.
        [[nodiscard]] usize RowCount() const noexcept { return m_rows.Size(); }
        [[nodiscard]] bool IsHeader(i32 row) const { return row >= 0 && static_cast<usize>(row) < m_rows.Size() && m_rows[static_cast<usize>(row)].Header; }
        [[nodiscard]] StringView RowText(i32 row) const
        {
            if (row < 0 || static_cast<usize>(row) >= m_rows.Size())
                return {};
            const Row& r = m_rows[static_cast<usize>(row)];
            return r.Header ? r.Section.AsView() : m_request.Items[r.Item].Name.AsView();
        }
        /// A tree's row: how deep it is, and whether it is shown only as where a match is.
        [[nodiscard]] u32 RowDepth(i32 row) const { return IsItemRow(row) ? m_request.Items[m_rows[static_cast<usize>(row)].Item].Depth : 0u; }
        [[nodiscard]] bool IsContext(i32 row) const { return IsItemRow(row) && m_rows[static_cast<usize>(row)].Context; }
        /// The lit item's key (Enter's); empty with none.
        [[nodiscard]] StringView LitKey() const
        {
            const i32 row = m_list != nullptr ? m_list->GetSelectedItem() : -1;
            return IsItemRow(row) ? m_request.Items[m_rows[static_cast<usize>(row)].Item].Key.AsView() : StringView{};
        }
        /// Moves the light `by` items, past the headings; it stays at the ends.
        void Step(i32 by)
        {
            if (m_list == nullptr || m_rows.IsEmpty())
                return;
            i32 row = m_list->GetSelectedItem();
            if (row < 0)
            {
                Light(FirstItemFrom(0, 1));
                return;
            }
            const i32 step = by < 0 ? -1 : 1;
            for (i32 left = by < 0 ? -by : by; left > 0; --left)
            {
                const i32 next = FirstItemFrom(row + step, step);
                if (next < 0)
                    break;
                row = next;
            }
            Light(row);
        }
        /// Picks the lit item; with none lit, nothing.
        void PickLit() { PickRow(m_list != nullptr ? m_list->GetSelectedItem() : -1); }
        [[nodiscard]] StringView Hint() const { return m_hint != nullptr ? m_hint->GetText() : StringView{}; }
        /// Whether it shows its state (nothing to pick, nothing matching) instead of rows, and what
        /// that says.
        [[nodiscard]] bool ShowsState() const { return m_state != nullptr && m_state->IsDisplayed(); }
        [[nodiscard]] StringView StateTitle() const
        {
            auto* title = Find<gui::Label>(u8"picker-state-title");
            return title != nullptr ? title->GetText() : StringView{};
        }
        [[nodiscard]] gui::ListView* List() const noexcept { return m_list; }
        [[nodiscard]] gui::Button* CloseButton() const { return Find<gui::Button>(u8"picker-close"); }
        [[nodiscard]] gui::Button* StateVerbButton() const { return Find<gui::Button>(u8"picker-state-verb"); }
        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }

    private:
        static constexpr f32 kRowEstimate = 26.0f;
        static constexpr u64 kHeaderBit = 1ull << 63;

        struct Row
        {
            bool Header = false;
            usize Item = 0;  // an item's, in the request
            String Section;  // its section (a heading's text)
            bool Context = false; // a tree's item shown only as where a match is
        };

        // The shown rows as the list's model: a heading cannot be selected; an id follows the row.
        class Model final : public gui::IModel
        {
        public:
            explicit Model(Picker& picker) : m_picker(picker) {}
            [[nodiscard]] usize RowCount(const gui::ModelIndex& parent = {}) const override { return parent.IsValid() ? 0 : m_picker.m_rows.Size(); }
            [[nodiscard]] usize ColumnCount() const override { return 1; }
            [[nodiscard]] gui::Variant Data(const gui::ModelIndex& index, gui::ModelRole role = gui::ModelRole::Display) const override
            {
                return role == gui::ModelRole::Display ? gui::Variant(m_picker.RowText(index.Row)) : gui::Variant{};
            }
            [[nodiscard]] bool IsSelectable(const gui::ModelIndex& index) const override { return m_picker.IsItemRow(index.Row); }
            [[nodiscard]] gui::ItemId IdOf(const gui::ModelIndex& index) const override
            {
                if (index.Row < 0 || static_cast<usize>(index.Row) >= m_picker.m_rows.Size())
                    return 0;
                const Row& row = m_picker.m_rows[static_cast<usize>(index.Row)];
                const u64 section = Hash<StringView>{}(row.Section.AsView());
                return row.Header ? (section | kHeaderBit) : ((section * 31u) ^ Hash<StringView>{}(m_picker.m_request.Items[row.Item].Key.AsView())) & ~kHeaderBit;
            }
            void Reset() { NotifyReset(); }

        private:
            Picker& m_picker;
        };

        [[nodiscard]] bool IsItemRow(i32 row) const { return row >= 0 && static_cast<usize>(row) < m_rows.Size() && !m_rows[static_cast<usize>(row)].Header; }
        // The first item row from `row` on in `step`'s direction; -1 when there is none.
        [[nodiscard]] i32 FirstItemFrom(i32 row, i32 step) const
        {
            for (; row >= 0 && static_cast<usize>(row) < m_rows.Size(); row += step)
                if (!m_rows[static_cast<usize>(row)].Header)
                    return row;
            return -1;
        }
        void Light(i32 row)
        {
            if (m_list != nullptr && IsItemRow(row))
            {
                m_list->SetSelectedItem(row);
                ShowHint(); // lit from code: the list's own callback does not hear it
            }
        }

        // The rows for `query` (case folded; a name that starts with it before one that holds it,
        // or whose detail does), favourites first, then each section in the order it first came.
        void Filter(StringView query)
        {
            m_rows.Clear();
            if (m_request.Tree)
            {
                FilterTree(query);
                return;
            }
            const auto matches = [query](const PickerItem& item) { return item.Name.AsView().ContainsIgnoreCase(query) || item.Detail.AsView().ContainsIgnoreCase(query); };
            const auto starts = [query](const PickerItem& item)
            {
                const StringView name = item.Name.AsView();
                return query.IsEmpty() || name.SubStr(0, Min(query.Size(), name.Size())).ContainsIgnoreCase(query);
            };
            const auto addSection = [this, &matches, &starts](StringView section, bool favourites)
            {
                bool headed = false;
                for (int pass = 0; pass < 2; ++pass)
                    for (usize i = 0; i < m_request.Items.Size(); ++i)
                    {
                        const PickerItem& item = m_request.Items[i];
                        const bool member = favourites ? item.Favourite : item.Section.AsView() == section;
                        if (!member || !matches(item) || starts(item) != (pass == 0))
                            continue;
                        if (!headed && !section.IsEmpty())
                            m_rows.PushBack(Row{true, 0, String(section)});
                        headed = true;
                        m_rows.PushBack(Row{false, i, String(section)});
                    }
            };
            addSection(kFavourites, true);
            Array<String> sections;
            for (const PickerItem& item : m_request.Items)
            {
                bool known = false;
                for (const String& section : sections)
                    known = known || section == item.Section;
                if (!known)
                    sections.PushBack(item.Section);
            }
            for (const String& section : sections)
                addSection(section.AsView(), false);
            Shown(query);
        }
        // A tree's rows for `query`: its items in their order, those matching by name and the
        // groups above each, so a match reads where it is (two of one name under different
        // parents apart). No sections, no favourites: the tree is the order.
        void FilterTree(StringView query)
        {
            const usize count = m_request.Items.Size();
            Array<bool> shown(m_allocator);
            Array<bool> matched(m_allocator);
            shown.Resize(count, false);
            matched.Resize(count, false);
            for (usize i = 0; i < count; ++i)
            {
                if (!m_request.Items[i].Name.AsView().ContainsIgnoreCase(query))
                    continue;
                matched[i] = shown[i] = true;
                u32 depth = m_request.Items[i].Depth;
                for (usize up = i; up-- > 0 && depth > 0;)
                    if (m_request.Items[up].Depth < depth)
                    {
                        shown[up] = true;
                        depth = m_request.Items[up].Depth;
                    }
            }
            for (usize i = 0; i < count; ++i)
                if (shown[i])
                    m_rows.PushBack(Row{false, i, String{}, !query.IsEmpty() && !matched[i]});
            Shown(query);
        }
        // After filtering: the list told, the state shown, the current value lit, else the first
        // match.
        void Shown(StringView query)
        {
            m_model.Reset();
            ShowState(query);
            i32 lit = -1;
            for (usize row = 0; row < m_rows.Size() && lit < 0; ++row)
                if (!m_rows[row].Header && !m_rows[row].Context && !m_request.Current.IsEmpty() && m_request.Items[m_rows[row].Item].Key == m_request.Current)
                    lit = static_cast<i32>(row);
            if (m_list != nullptr)
                m_list->ClearSelection();
            if (lit < 0)
                for (usize row = 0; row < m_rows.Size() && lit < 0; ++row)
                    if (!m_rows[row].Header && !m_rows[row].Context)
                        lit = static_cast<i32>(row);
            Light(lit);
        }
        void ShowState(StringView query)
        {
            const bool none = m_request.Items.IsEmpty();
            const bool unmatched = !none && m_rows.IsEmpty();
            if (m_list != nullptr)
                m_list->SetDisplayed(!none && !unmatched);
            if (m_state != nullptr)
                m_state->SetDisplayed(none || unmatched);
            if (auto* icon = Find<gui::Image>(u8"picker-state-icon"))
                icon->SetSource(String(unmatched ? StringView(u8"Editor/Icons/search.svg") : StringView(u8"Editor/Icons/folder-open.svg")));
            if (auto* title = Find<gui::Label>(u8"picker-state-title"))
                title->SetText(unmatched ? Format(u8"Nothing matches {}", query).AsView() : m_request.EmptyTitle.AsView());
            if (auto* text = Find<gui::Label>(u8"picker-state-text"))
            {
                text->SetText(m_request.EmptyText.AsView());
                text->SetDisplayed(none && !m_request.EmptyText.IsEmpty());
            }
            if (gui::Button* verb = StateVerbButton())
            {
                verb->SetText(unmatched ? StringView(u8"Clear the search") : m_request.EmptyVerb.AsView());
                verb->SetDisplayed(unmatched || (none && m_request.OnEmptyVerb && !m_request.EmptyVerb.IsEmpty()));
            }
            if (m_hint != nullptr)
            {
                StringView current;
                for (const PickerItem& item : m_request.Items)
                    if (!m_request.Current.IsEmpty() && item.Key == m_request.Current)
                        current = item.Name.AsView();
                m_keysHint = current.IsEmpty() ? String(u8"Enter picks, Esc closes") : Format(u8"Enter picks, Esc keeps {}", current);
                ShowHint();
            }
        }
        // The foot: the lit item's description where it has one (read as the arrows move), else
        // what the keys do.
        void ShowHint()
        {
            if (m_hint == nullptr)
                return;
            const i32 lit = m_list != nullptr ? m_list->GetSelectedItem() : -1;
            const StringView description = IsItemRow(lit) ? m_request.Items[m_rows[static_cast<usize>(lit)].Item].Description.AsView() : StringView{};
            if (!description.IsEmpty())
            {
                m_hint->SetText(description);
                m_hint->AddClass(u8"described");
            }
            else
            {
                m_hint->SetText(m_keysHint.AsView());
                m_hint->RemoveClass(u8"described");
            }
        }
        void StateVerb()
        {
            if (!m_request.Items.IsEmpty())
            {
                SetQuery(StringView{});
                if (m_search != nullptr)
                    m_search->RequestFocus();
                return;
            }
            Function<void()> verb = Move(m_request.OnEmptyVerb);
            Close();
            if (verb)
                verb();
        }
        void PickRow(i32 row)
        {
            if (IsItemRow(row))
                Pick(m_request.Items[m_rows[static_cast<usize>(row)].Item].Key);
        }
        // It closes first: what is picked may open something of its own.
        void Pick(const String& key)
        {
            Function<void(StringView)> picked = Move(m_request.OnPicked);
            const String chosen = key;
            Close();
            if (picked)
                picked(chosen.AsView());
        }

        [[nodiscard]] RefPtr<gui::Node> MakeRow(u32 type)
        {
            if (type == 1u)
            {
                RefPtr<gui::Label> heading = MakeRef<gui::Label>(m_allocator);
                heading->AddClass(u8"picker-group");
                return heading;
            }
            return MakeRef<PickerRow>(m_allocator);
        }
        void BindRow(gui::Node& node, i32 row)
        {
            if (row < 0 || static_cast<usize>(row) >= m_rows.Size())
                return;
            const Row& r = m_rows[static_cast<usize>(row)];
            if (r.Header)
            {
                if (auto* heading = Cast<gui::Label>(&node))
                    heading->SetText(r.Section.AsView());
            }
            else if (auto* shown = Cast<PickerRow>(&node))
            {
                const PickerItem& item = m_request.Items[r.Item];
                shown->Show(item, !m_request.Current.IsEmpty() && item.Key == m_request.Current, r.Context);
            }
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

        IAllocator& m_allocator;
        RefPtr<gui::Node> m_content;
        gui::TextField* m_search = nullptr;
        gui::ListView* m_list = nullptr;
        gui::UIWidget* m_state = nullptr;
        gui::Label* m_hint = nullptr;
        String m_keysHint; // what the keys do, the foot's when the lit item says nothing
        gui::SceneRoot* m_root = nullptr; // what it opened over
        PickerRequest m_request;
        Array<Row> m_rows;
        Model m_model;
    };

    RTTI_DEFINE_OBJECT(PickerRow, "rtti::editor::editor::app")
}
