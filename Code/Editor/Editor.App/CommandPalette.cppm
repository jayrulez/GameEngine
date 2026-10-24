// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :command_palette partition.
//
// CommandPalette: every action by name (Documentation/Specs/editor-gui.md, 5.1, layer 10). Its
// frame is markup (Data/Editor/CommandPalette.gml: the kit's picker, a search field over a list
// and a hint), its rows the registry's declarations filtered by what is typed (ASCII case folded):
// labels that start with it first, then labels that hold it, then a description or id that does.
// A row shows the action's label, its menu path and its chord; one disabled over the active page
// is dimmed and refuses with a line in the hint. Up and Down move along the rows, Enter or a click
// runs the lit one through the registry and closes the palette, Escape or a press outside closes
// it. It opens as a modal popup near the top of the window, the field focused.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:command_palette;

import foundation.core;
import foundation.gui;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// One action's row: its label, where it is in the menus, its chord. The pointer over it lights
    /// it; a click runs it (its palette's callbacks, by the row's place).
    class CommandPaletteRow final : public gui::UIWidget
    {
        RTTI_OBJECT(CommandPaletteRow, gui::UIWidget)
    public:
        CommandPaletteRow(i32 index, Function<void(i32)> light, Function<void(i32)> run)
            : m_index(index), m_light(foundation::core::Move(light)), m_run(foundation::core::Move(run))
        {
            AddClass(u8"palette-row");
            m_label = MakeRef<gui::Label>(MemoryAllocator());
            m_label->AddClass(u8"row-name");
            m_where = MakeRef<gui::Label>(MemoryAllocator());
            m_where->AddClass(u8"palette-where");
            m_chord = MakeRef<gui::Label>(MemoryAllocator());
            m_chord->AddClass(u8"key");
            gui::Label* const parts[] = {m_label.Get(), m_where.Get(), m_chord.Get()};
            for (gui::Label* part : parts)
            {
                part->SetHitTestVisible(false);
                AddChild(part);
            }
        }
        [[nodiscard]] i32 Index() const noexcept { return m_index; }
        void Show(StringView label, StringView where, StringView chord, bool enabled)
        {
            m_label->SetText(label);
            m_where->SetText(where);
            m_chord->SetText(chord);
            m_chord->SetDisplayed(!chord.IsEmpty());
            if (enabled)
                RemoveClass(u8"disabled");
            else
                AddClass(u8"disabled");
        }

    protected:
        void OnMouseEnter(const gui::MouseEvent&) override
        {
            if (m_light)
                m_light(m_index);
        }
        void OnMouseClick(const gui::MouseEvent& event) override
        {
            if (event.Button != gui::MouseButton::Left)
                return;
            event.Handled = true;
            if (m_run)
                m_run(m_index);
        }

    private:
        i32 m_index;
        Function<void(i32)> m_light;
        Function<void(i32)> m_run;
        RefPtr<gui::Label> m_label;
        RefPtr<gui::Label> m_where;
        RefPtr<gui::Label> m_chord;
    };

    class CommandPalette
    {
    public:
        /// The palette over `actions`, its frame `content` (the markup's: `palette-search`,
        /// `palette-list`, `palette-hint` by id).
        CommandPalette(IAllocator& allocator, EditorActionRegistry& actions, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_actions(&actions), m_content(Move(content)), m_shown(allocator), m_rows(allocator)
        {
            if (!m_content)
                return;
            m_search = Cast<gui::TextField>(Find(*m_content, u8"palette-search"));
            m_list = Find(*m_content, u8"palette-list");
            m_hint = Cast<gui::Label>(Find(*m_content, u8"palette-hint"));
            if (m_hint != nullptr)
                m_idleHint = String(m_hint->GetText());
            CommandPalette* self = this;
            if (m_search != nullptr)
                m_search->SetOnTextChanging([self](StringView text) { self->Filter(text); });
            // Up, Down and Enter before the search field takes them.
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
                                                      (void)self->RunLit();
                                                      e.Handled = true;
                                                  }
                                              },
                                              true);
        }
        CommandPalette(const CommandPalette&) = delete;
        CommandPalette& operator=(const CommandPalette&) = delete;
        ~CommandPalette() { Close(); }

        /// Opens it over `root`, near its top, the query empty and the field focused; false with
        /// no frame or a root in no GUI.
        bool Open(gui::SceneRoot& root)
        {
            gui::EventDispatcher* dispatcher = root.GetEventDispatcher();
            if (!m_content || dispatcher == nullptr || IsOpen())
                return false;
            m_root = &root;
            if (m_search != nullptr)
                m_search->SetText(StringView{});
            Filter(StringView{});
            gui::PopupOptions options;
            options.Kind = gui::PopupKind::Auto;
            options.Modal = true;
            options.TakesFocus = true;
            options.FitContent = true;
            options.HasAnchorRect = true;
            options.AnchorRect = gui::Rect{0.0f, 0.0f, root.GetSize().x, root.GetSize().y * kTopShare};
            options.Placement.Side = gui::PopupSide::Below;
            options.Placement.Align = gui::PopupAlign::Center;
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

        /// Filters for `text`, as typing it in the field does.
        void SetQuery(StringView text)
        {
            if (m_search != nullptr)
                m_search->SetText(text);
            Filter(text);
        }
        /// The actions shown now, in their order, and the lit one (Enter's; -1 with none).
        [[nodiscard]] Span<const EditorActionDeclaration* const> Shown() const noexcept { return Span<const EditorActionDeclaration* const>{m_shown.Data(), m_shown.Size()}; }
        [[nodiscard]] i32 Lit() const noexcept { return m_lit; }
        void Light(i32 index)
        {
            m_lit = m_shown.IsEmpty() ? -1 : Clamp(index, 0, static_cast<i32>(m_shown.Size()) - 1);
            for (const RefPtr<CommandPaletteRow>& row : m_rows)
            {
                if (row->Index() == m_lit)
                {
                    row->AddClass(u8"current");
                    if (m_list != nullptr)
                        m_list->ScrollIntoView(*row);
                }
                else
                    row->RemoveClass(u8"current");
            }
        }
        void Step(i32 by) { Light(m_lit + by); }
        /// Runs the lit action: the palette closes first (an action may open a dialog of its own).
        /// One disabled over the active page stays, its refusal in the hint; with none lit, nothing.
        Status RunLit() { return Run(m_lit); }
        Status Run(i32 index)
        {
            if (index < 0 || static_cast<usize>(index) >= m_shown.Size())
                return Status{ErrorCode::NotFound};
            const EditorActionDeclaration* action = m_shown[static_cast<usize>(index)];
            if (!m_actions->IsEnabled(action->id.AsView()))
            {
                SetHint(Format(u8"{} is not available now.", action->label).AsView());
                return Status{ErrorCode::InvalidArgument};
            }
            const String id = action->id; // the registry may change as it runs
            Close();
            return m_actions->Execute(id.AsView());
        }
        [[nodiscard]] StringView Hint() const { return m_hint != nullptr ? m_hint->GetText() : StringView{}; }

    private:
        static constexpr f32 kTopShare = 0.12f; // how far down the window it opens

        void Filter(StringView query)
        {
            m_shown.Clear();
            Array<const EditorActionDeclaration*> holds;
            Array<const EditorActionDeclaration*> elsewhere;
            for (const EditorActionDeclaration& action : m_actions->Actions())
            {
                const StringView label = action.label.AsView();
                if (label.ContainsIgnoreCase(query))
                {
                    if (query.IsEmpty() || label.SubStr(0, Min(query.Size(), label.Size())).ContainsIgnoreCase(query))
                        m_shown.PushBack(&action); // a label that starts with it
                    else
                        holds.PushBack(&action);
                }
                else if (action.description.AsView().ContainsIgnoreCase(query) || action.id.AsView().ContainsIgnoreCase(query))
                    elsewhere.PushBack(&action);
            }
            for (const EditorActionDeclaration* action : holds)
                m_shown.PushBack(action);
            for (const EditorActionDeclaration* action : elsewhere)
                m_shown.PushBack(action);
            ShowRows();
            SetHint(m_shown.IsEmpty() ? Format(u8"Nothing matches {}.", query).AsView() : m_idleHint.AsView());
            Light(0);
        }
        void ShowRows()
        {
            if (m_list == nullptr)
                return;
            while (m_rows.Size() < m_shown.Size())
            {
                CommandPalette* self = this;
                m_rows.PushBack(MakeRef<CommandPaletteRow>(m_allocator, static_cast<i32>(m_rows.Size()), [self](i32 at) { self->Light(at); },
                                                           [self](i32 at) { (void)self->Run(at); }));
                m_list->AddChild(m_rows.Back().Get());
            }
            while (m_rows.Size() > m_shown.Size())
            {
                m_list->RemoveChild(m_rows.Back().Get());
                m_rows.RemoveAt(m_rows.Size() - 1);
            }
            for (usize i = 0; i < m_shown.Size(); ++i)
            {
                const EditorActionDeclaration& action = *m_shown[i];
                m_rows[i]->Show(action.label.AsView(), action.menuPath.AsView(), FormatShortcut(m_actions->Shortcut(action.id.AsView())).AsView(),
                                m_actions->IsEnabled(action.id.AsView()));
            }
        }
        void SetHint(StringView text)
        {
            if (m_hint != nullptr)
                m_hint->SetText(text);
        }
        static gui::Node* Find(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = Find(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        IAllocator& m_allocator;
        EditorActionRegistry* m_actions;
        RefPtr<gui::Node> m_content;
        gui::TextField* m_search = nullptr;
        gui::Node* m_list = nullptr;
        gui::Label* m_hint = nullptr;
        String m_idleHint;
        gui::SceneRoot* m_root = nullptr; // what it opened over
        Array<const EditorActionDeclaration*> m_shown;
        Array<RefPtr<CommandPaletteRow>> m_rows;
        i32 m_lit = -1;
    };

    RTTI_DEFINE_OBJECT(CommandPaletteRow, "rtti::editor::editor::app")
}
