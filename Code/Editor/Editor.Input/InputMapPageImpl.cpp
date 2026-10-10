// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Input - the `editor.input` module. See InputMapPage.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"
#include <cstdlib>

module editor.input;

import foundation.core;
import foundation.content;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import foundation.input;
import input.pipeline;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace input = foundation::input;
namespace runtime = foundation::runtime;
namespace ui = foundation::ui;

namespace editor
{
    namespace
    {
        constexpr Color kDimText{0.62f, 0.62f, 0.62f, 1.0f};
        constexpr Color kWarningText{0.91f, 0.69f, 0.29f, 1.0f};
        constexpr Color kAccent{0.31f, 0.56f, 0.95f, 1.0f};
        constexpr f32 kLabelWidth = 96.0f; // a settings row's label column

        [[nodiscard]] ui::DrawablePtr Rounded(IAllocator& allocator, Color fill, f32 radius, Color border,
                                              f32 borderWidth = 1.0f)
        {
            return ui::DrawablePtr(MakeRef<ui::RoundedRectDrawable>(allocator, fill, radius, border, borderWidth).Get());
        }

        [[nodiscard]] bool SourceFitsKind(input::ActionKind kind, input::BindingSource source)
        {
            input::BindingSource valid[8];
            const usize n = input::ValidSources(kind, valid);
            for (usize i = 0; i < n; ++i)
            {
                if (valid[i] == source)
                {
                    return true;
                }
            }
            return false;
        }

        /// What a hold, tap or double tap's seconds mean, before the field.
        [[nodiscard]] StringView SecondsLabel(input::InteractionKind kind)
        {
            return kind == input::InteractionKind::Hold ? StringView(u8"for") : StringView(u8"within");
        }
    }

    InputMapEditorPage::InputMapEditorPage(EditorContext& context, runtime::IApplicationHost&,
                                           foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()), m_context(&context), m_title(instance.Name())
    {
        RefPtr<ISerializable> object = instance.ReadObject();
        if (auto* asset = Cast<pipeline::InputMapAsset>(object.Get()))
        {
            m_map = asset->Map();
        }

        auto column = MakeRef<ui::FlexLayout>(Allocator());
        column->Direction = ui::Orientation::Vertical;

        // The top bar: what the map says (or what listening waits for), and the layout.
        {
            auto bar = Row(8.0f);
            bar->Padding = ui::Thickness{10, 6};
            m_status = MakeRef<ui::Label>(Allocator(), StringView(u8""));
            m_status->FontSize.SetValue(12.0f);
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.AlignSelf = ui::Align::Center;
            bar->AddView(m_status.Get(), grow);
            AddLabel(*bar, u8"Layout", 11.0f, true);
            m_layoutChoice = MakeRef<ui::ComboBox>(Allocator());
            (void)m_layoutChoice->AddItem(u8"Outline");
            (void)m_layoutChoice->AddItem(u8"Two panes");
            m_layoutChoice->SetSelectedIndex(0);
            InputMapEditorPage* self = this;
            m_layoutChoice->OnSelectionChanged.Add(
                [self](ui::ComboBox*, i32 index)
                { self->SetLayout(index == 1 ? Layout::TwoPanes : Layout::Outline); });
            ui::LayoutStyle fixed;
            fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(120.0f));
            fixed.AlignSelf = ui::Align::Center;
            bar->AddView(m_layoutChoice.Get(), fixed);
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column->AddView(bar.Get(), match);
            auto rule = MakeRef<ui::Separator>(Allocator());
            ui::LayoutStyle line;
            line.Width = ui::SizeSpec::Match();
            line.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(1.0f));
            column->AddView(rule.Get(), line);
        }

        m_body = MakeRef<ui::FlexLayout>(Allocator());
        m_body->Direction = ui::Orientation::Horizontal;
        {
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            column->AddView(m_body.Get(), grow);
        }
        const auto makeScroll = [this]()
        {
            auto scroll = MakeRef<ui::ScrollView>(Allocator());
            scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
            scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
            return scroll;
        };
        m_outlineScroll = makeScroll();
        m_listScroll = makeScroll();
        m_detailScroll = makeScroll();

        m_toolbar = MakeRef<app::PageToolbar>(Allocator(), *this, m_context->Actions());
        m_content = app::PageToolbar::Frame(Allocator(), *m_toolbar, *column);
        Rebuild();
    }

    Status InputMapEditorPage::Save()
    {
        String error;
        if (!input::ValidateInputMap(m_map, &error))
        {
            String message(u8"Input map invalid: ");
            message += error;
            m_context->Notify(NoticeKind::Error, message.AsView());
            return Status{ErrorCode::InvalidArgument};
        }
        foundation::content::Instance* instance =
            (m_context->Project() != nullptr) ? m_context->Project()->SourceDb().GetInstance(InstanceId()) : nullptr;
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        pipeline::InputMapAsset asset;
        asset.Map() = m_map;
        const Status written = instance->WriteObject(asset);
        if (written.IsOk())
        {
            ClearDirty();
        }
        return written;
    }

    void InputMapEditorPage::OnUpdate(runtime::IApplicationHost& host, f32)
    {
        if (m_toolbar.Get() != nullptr)
        {
            m_toolbar->Refresh(); // sync the page's actions to it each frame
        }
        if (!m_listening)
        {
            return;
        }
        auto* shellInput = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
        if (shellInput == nullptr)
        {
            return;
        }
        // Escape is a BINDABLE key, so it is not a cancel here - cancel is clicking the key cap
        // again. CaptureBinding below takes whatever qualifies.
        input::ShellInputSource devices(shellInput);
        input::Binding captured;
        if (input::CaptureBinding(devices, m_listenFilter, captured))
        {
            const usize set = m_listenSet;
            const usize action = m_listenAction;
            const usize binding = m_listenBinding;
            const i32 direction = m_listenDirection;
            m_listening = false;
            m_listenDirection = -1;
            Mutate([set, action, binding, direction, captured](input::InputMap& map)
                   { input_map_edit::ApplyCapture(map, set, action, binding, direction, captured); });
        }
    }

    void InputMapEditorPage::SetLayout(Layout layout)
    {
        if (m_layout == layout)
        {
            return;
        }
        m_layout = layout;
        m_layoutChoice->SetSelectedIndex(layout == Layout::TwoPanes ? 1 : 0);
        RequestRebuild();
    }

    void InputMapEditorPage::Select(usize set, isize action)
    {
        m_selectedSet = set;
        m_selectedAction = action;
        m_detailScroll->SetScrollY(0.0f);
        RequestRebuild();
    }

    bool InputMapEditorPage::IsSetCollapsed(usize set) const
    {
        if (set >= m_map.sets.Size())
        {
            return false;
        }
        for (const String& name : m_collapsed)
        {
            if (name == m_map.sets[set].name)
            {
                return true;
            }
        }
        return false;
    }

    void InputMapEditorPage::SetSetCollapsed(usize set, bool collapsed)
    {
        if (set >= m_map.sets.Size() || IsSetCollapsed(set) == collapsed)
        {
            return;
        }
        if (collapsed)
        {
            m_collapsed.PushBack(m_map.sets[set].name);
            return;
        }
        for (usize i = 0; i < m_collapsed.Size(); ++i)
        {
            if (m_collapsed[i] == m_map.sets[set].name)
            {
                m_collapsed.RemoveAt(i);
                return;
            }
        }
    }

    void InputMapEditorPage::AddBinding(usize set, usize action, input::BindingSource source)
    {
        Mutate(
            [set, action, source](input::InputMap& m)
            {
                if (input::Action* a = input_map_edit::ActionAt(m, set, action))
                {
                    a->bindings.PushBack(input_map_edit::FreshBinding(source));
                }
            });
    }

    void InputMapEditorPage::BeginListen(usize set, usize action, usize binding, i32 compositeDirection)
    {
        // The same key cap again cancels (Escape is a key a game binds, so it cannot cancel).
        if (m_listening && m_listenSet == set && m_listenAction == action && m_listenBinding == binding &&
            m_listenDirection == compositeDirection)
        {
            m_listening = false;
            m_listenDirection = -1;
            RequestRebuild();
            return;
        }
        m_listening = true;
        m_listenSet = set;
        m_listenAction = action;
        m_listenBinding = binding;
        m_listenDirection = compositeDirection;
        // Filter by the action's declared kind (a composite direction is always one key).
        const input::Action* target = input_map_edit::ActionAt(m_map, set, action);
        m_listenFilter = input_map_edit::FilterFor(target != nullptr ? target->kind : input::ActionKind::Button,
                                                   compositeDirection >= 0);
        // Deferred: this runs inside the key cap's click, which a rebuild would free.
        RequestRebuild();
    }

    void InputMapEditorPage::RequestRebuild()
    {
        ui::UIContext* ctx = (m_body.Get() != nullptr) ? m_body->Context : nullptr;
        if (ctx == nullptr)
        {
            Rebuild();
            return;
        } // not attached yet (setup) - safe to do now
        InputMapEditorPage* self = this;
        ctx->MutationQueueRef().QueueAction(Function<void()>{[self]() { self->Rebuild(); }});
    }

    void InputMapEditorPage::Rebuild()
    {
        // Keep the two-pane selection on something that exists.
        if (m_selectedSet >= m_map.sets.Size())
        {
            m_selectedSet = m_map.sets.IsEmpty() ? 0 : m_map.sets.Size() - 1;
            m_selectedAction = -1;
        }
        if (!m_map.sets.IsEmpty() && m_selectedAction >= 0 &&
            static_cast<usize>(m_selectedAction) >= m_map.sets[m_selectedSet].actions.Size())
        {
            m_selectedAction = static_cast<isize>(m_map.sets[m_selectedSet].actions.Size()) - 1;
        }
        m_body->RemoveAllViews();
        if (m_layout == Layout::Outline)
        {
            BuildOutline();
        }
        else
        {
            BuildTwoPanes();
        }
        RefreshStatus();
        m_content->Invalidate();
    }

    // === The outline: each set a collapsible section of its actions =========================

    void InputMapEditorPage::BuildOutline()
    {
        InputMapEditorPage* self = this;
        app::EditorIcons& icons = app::EditorIcons::Get();
        auto page = Column(10.0f);
        page->Padding = ui::Thickness{14, 10};
        {
            auto head = Row();
            AddLabel(*head, u8"Action sets", 14.0f);
            AddLabel(*head, Format(u8"{}", m_map.sets.Size()).AsView(), 12.0f, true);
            auto spacer = MakeRef<ui::View>(Allocator());
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            head->AddView(spacer.Get(), grow);
            auto add = MakeRef<ui::Button>(Allocator(), StringView(u8"Add Set"));
            add->OnClick.Add(
                [self](ui::ButtonBase*)
                {
                    self->Mutate(
                        [](input::InputMap& m)
                        {
                            input::ActionSet set;
                            set.name = String(u8"NewSet");
                            m.sets.PushBack(static_cast<input::ActionSet&&>(set));
                        });
                });
            head->AddView(add.Get());
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            page->AddView(head.Get(), match);
        }
        if (m_map.sets.IsEmpty())
        {
            AddNote(*page, u8"No action sets. A set groups the actions a game turns on together "
                           u8"(gameplay, menus); add one to begin.");
        }

        for (usize s = 0; s < m_map.sets.Size(); ++s)
        {
            const input::ActionSet& set = m_map.sets[s];
            auto section = MakeRef<ui::Expander>(Allocator());
            section->HeaderHeight.SetValue(32.0f);
            // A border round the whole section, band and body (user): a set reads as one block.
            section->SetStyle(ui::StyleProperty::BorderWidth, 1.0f);
            section->SetStyle(ui::StyleProperty::BorderColor, Color{1.0f, 1.0f, 1.0f, 0.14f});
            section->SetIsExpanded(!IsSetCollapsed(s));
            section->OnExpandedChanged.Add([self, s](ui::Expander*, bool expanded)
                                           { self->SetSetCollapsed(s, !expanded); });

            // The header: the set's name, renamed in place, then its priority and its verbs.
            auto name = MakeRef<ui::EditableLabel>(Allocator());
            name->SetText(set.name.AsView());
            name->FontSize.SetValue(Optional<f32>(13.5f));
            name->TooltipText = String(u8"Double-click to rename the set");
            name->OnRenameCommitted.Add(
                [self, s](ui::EditableLabel*, StringView value)
                {
                    if (value.IsEmpty())
                    {
                        return;
                    }
                    const String renamed(value);
                    self->Mutate(
                        [s, renamed](input::InputMap& m)
                        {
                            if (input::ActionSet* target = input_map_edit::SetAt(m, s))
                            {
                                target->name = renamed;
                            }
                        });
                });
            section->SetHeaderTitle(name.Get());
            auto verbs = Row(6.0f);
            AddLabel(*verbs, Format(u8"{} actions", set.actions.Size()).AsView(), 11.0f, true);
            AddNumber(*verbs, u8"Priority", u8"When two active sets bind the same input, the higher priority takes it",
                      static_cast<f64>(set.priority), -100.0, 100.0, 0, Format(u8"priority/{}", s).AsView(),
                      [s](input::InputMap& m, f64 v)
                      {
                          if (input::ActionSet* target = input_map_edit::SetAt(m, s))
                          {
                              target->priority = static_cast<i32>(v);
                          }
                      });
            AddIcon(*verbs, icons.add.Get(), u8"Add an action to this set",
                    [self, s]()
                    {
                        self->SetSetCollapsed(s, false);
                        self->Mutate(
                            [s](input::InputMap& m)
                            {
                                if (input::ActionSet* target = input_map_edit::SetAt(m, s))
                                {
                                    input::Action action;
                                    action.name = String(u8"NewAction");
                                    target->actions.PushBack(static_cast<input::Action&&>(action));
                                }
                            });
                    });
            AddIcon(*verbs, icons.remove.Get(), u8"Remove the set and its actions",
                    [self, s]()
                    {
                        self->Mutate(
                            [s](input::InputMap& m)
                            {
                                if (s < m.sets.Size())
                                {
                                    m.sets.RemoveAt(s);
                                }
                            });
                    });
            section->SetHeaderActions(verbs.Get());

            // The body: each action a card.
            auto body = Column(8.0f);
            body->Padding = ui::Thickness{8, 4, 8, 8};
            if (set.actions.IsEmpty())
            {
                AddNote(*body, u8"No actions. Add one with + in the header.");
            }
            for (usize a = 0; a < set.actions.Size(); ++a)
            {
                auto card = Column(8.0f);
                card->Padding = ui::Thickness{12, 10};
                BuildActionHeader(*card, s, a, true);
                BuildBindings(*card, s, a);
                BuildProcessing(*card, s, a);
                ui::LayoutStyle match;
                match.Width = ui::SizeSpec::Match();
                body->AddView(Card(*card).Get(), match);
            }
            section->SetContent(body.Get());
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            page->AddView(section.Get(), match);
        }

        m_outlineScroll->RemoveAllViews();
        {
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            m_outlineScroll->AddView(page.Get(), match);
        }
        ui::LayoutStyle fill;
        fill.FlexGrow = 1.0f;
        fill.Height = ui::SizeSpec::Match();
        m_body->AddView(m_outlineScroll.Get(), fill);
    }

    // === Two panes: the sets and actions listed, the selected one edited =====================

    void InputMapEditorPage::BuildTwoPanes()
    {
        InputMapEditorPage* self = this;
        app::EditorIcons& icons = app::EditorIcons::Get();
        const bool any = !m_map.sets.IsEmpty();

        // --- The list ---
        auto side = Column(0.0f);
        {
            auto bar = Row(4.0f);
            bar->Padding = ui::Thickness{8, 6};
            AddLabel(*bar, u8"Sets and actions", 12.0f, true);
            auto spacer = MakeRef<ui::View>(Allocator());
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            bar->AddView(spacer.Get(), grow);
            AddIcon(*bar, icons.add.Get(), u8"Add a set",
                    [self]()
                    {
                        self->Mutate(
                            [](input::InputMap& m)
                            {
                                input::ActionSet set;
                                set.name = String(u8"NewSet");
                                m.sets.PushBack(static_cast<input::ActionSet&&>(set));
                            });
                        self->Select(self->m_map.sets.Size() - 1, -1);
                    });
            ui::IconButton* addAction = AddIcon(*bar, icons.copy.Get(), u8"Add an action to the selected set",
                                                [self]()
                                                {
                                                    const usize s = self->m_selectedSet;
                                                    self->Mutate(
                                                        [s](input::InputMap& m)
                                                        {
                                                            if (input::ActionSet* set = input_map_edit::SetAt(m, s))
                                                            {
                                                                input::Action action;
                                                                action.name = String(u8"NewAction");
                                                                set->actions.PushBack(
                                                                    static_cast<input::Action&&>(action));
                                                            }
                                                        });
                                                    if (s < self->m_map.sets.Size())
                                                    {
                                                        self->Select(s, static_cast<isize>(
                                                                            self->m_map.sets[s].actions.Size()) -
                                                                            1);
                                                    }
                                                });
            addAction->IsEnabled = any;
            ui::IconButton* remove = AddIcon(*bar, icons.remove.Get(), u8"Remove the selected set or action",
                                             [self]()
                                             {
                                                 const usize s = self->m_selectedSet;
                                                 const isize a = self->m_selectedAction;
                                                 self->Mutate(
                                                     [s, a](input::InputMap& m)
                                                     {
                                                         if (a < 0)
                                                         {
                                                             if (s < m.sets.Size())
                                                             {
                                                                 m.sets.RemoveAt(s);
                                                             }
                                                         }
                                                         else if (input::ActionSet* set = input_map_edit::SetAt(m, s);
                                                                  set != nullptr &&
                                                                  static_cast<usize>(a) < set->actions.Size())
                                                         {
                                                             set->actions.RemoveAt(static_cast<usize>(a));
                                                         }
                                                     });
                                             });
            remove->IsEnabled = any;
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            side->AddView(bar.Get(), match);
        }
        auto list = Column(1.0f);
        list->Padding = ui::Thickness{6, 2, 6, 8};
        const auto listRow = [&](StringView badge, StringView text, StringView detail, bool selected, f32 indent,
                                 bool strong, Function<void()> onClick)
        {
            auto content = Row(6.0f);
            content->Padding = ui::Thickness{indent, 0, 0, 0};
            if (!badge.IsEmpty())
            {
                auto chip = MakeRef<ui::Label>(Allocator(), badge);
                chip->FontSize.SetValue(9.5f);
                chip->TextColor.SetValue(Optional<Color>(kDimText));
                auto pill = MakeRef<ui::Panel>(Allocator());
                pill->SetStyle(ui::StyleProperty::Background,
                               Rounded(Allocator(), Color{1, 1, 1, 0.06f}, 3.0f, Color{1, 1, 1, 0.12f}));
                pill->SetStyle(ui::StyleProperty::Padding, ui::Thickness{4.0f, 1.0f});
                pill->AddView(chip.Get());
                ui::LayoutStyle fixed;
                fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(26.0f));
                fixed.AlignSelf = ui::Align::Center;
                content->AddView(pill.Get(), fixed);
            }
            ui::Label* label = AddLabel(*content, text, strong ? 12.5f : 12.0f);
            (void)label;
            if (!detail.IsEmpty())
            {
                AddLabel(*content, detail, 10.5f, true);
            }
            // A toggle, checked while its row is the selection (a click selects; the rebuild that
            // follows checks the new row).
            auto row = MakeRef<ui::ToggleButton>(Allocator());
            row->SetContent(RefPtr<ui::View>(content.Get()));
            row->SetStyle(ui::StyleProperty::Padding, ui::Thickness{6.0f, 4.0f});
            row->IsChecked.SetSilent(selected);
            auto look = MakeRef<ui::StateListDrawable>(Allocator());
            const Color none{0, 0, 0, 0};
            const Color picked{kAccent.r, kAccent.g, kAccent.b, 0.28f};
            const Color pickedHover{kAccent.r, kAccent.g, kAccent.b, 0.34f};
            look->Set(ui::ControlState::Normal, Rounded(Allocator(), none, 4.0f, none, 0.0f));
            look->Set(ui::ControlState::Hover, Rounded(Allocator(), Color{1, 1, 1, 0.05f}, 4.0f, none, 0.0f));
            look->Set(ui::ControlState::Checked, Rounded(Allocator(), picked, 4.0f, none, 0.0f));
            look->Set(ui::ControlState::Checked | ui::ControlState::Hover,
                      Rounded(Allocator(), pickedHover, 4.0f, none, 0.0f));
            row->SetStyle(ui::StyleProperty::Background, RefPtr<ui::Drawable>(look.Get()));
            // A checked toggle draws its CheckedBackground (the theme's accent) first: this look too.
            row->SetStyle(ui::StyleProperty::CheckedBackground, RefPtr<ui::Drawable>(look.Get()));
            row->OnClick.Add([fn = Move(onClick)](ui::ButtonBase*) { fn(); });
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            list->AddView(row.Get(), match);
        };
        for (usize s = 0; s < m_map.sets.Size(); ++s)
        {
            const input::ActionSet& set = m_map.sets[s];
            listRow({}, set.name.AsView(), Format(u8"priority {}", set.priority).AsView(),
                    m_selectedSet == s && m_selectedAction < 0, 0.0f, true,
                    [self, s]() { self->Select(s, -1); });
            for (usize a = 0; a < set.actions.Size(); ++a)
            {
                const input::Action& action = set.actions[a];
                const StringView badge = action.kind == input::ActionKind::Axis2D   ? StringView(u8"2D")
                                         : action.kind == input::ActionKind::Axis1D ? StringView(u8"1D")
                                                                                    : StringView(u8"B");
                listRow(badge, action.name.AsView(), {},
                        m_selectedSet == s && m_selectedAction == static_cast<isize>(a), 14.0f, false,
                        [self, s, a]() { self->Select(s, static_cast<isize>(a)); });
            }
        }
        if (!any)
        {
            AddNote(*list, u8"No action sets yet: add one with +.");
        }
        m_listScroll->RemoveAllViews();
        {
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            m_listScroll->AddView(list.Get(), match);
        }
        {
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            side->AddView(m_listScroll.Get(), grow);
        }
        {
            ui::LayoutStyle fixed;
            fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(240.0f));
            fixed.Height = ui::SizeSpec::Match();
            m_body->AddView(side.Get(), fixed);
        }
        {
            auto rule = MakeRef<ui::Separator>(Allocator());
            rule->Orientation.SetValue(ui::Orientation::Vertical);
            ui::LayoutStyle line;
            line.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(1.0f));
            line.Height = ui::SizeSpec::Match();
            m_body->AddView(rule.Get(), line);
        }

        // --- The selected set or action ---
        auto detail = Column(10.0f);
        detail->Padding = ui::Thickness{16, 12};
        if (!any)
        {
            AddNote(*detail, u8"A set groups the actions a game turns on together (gameplay, menus). "
                             u8"Add one on the left to begin.");
        }
        else if (m_selectedAction < 0)
        {
            BuildSetFields(*detail, m_selectedSet);
        }
        else
        {
            const usize a = static_cast<usize>(m_selectedAction);
            BuildActionHeader(*detail, m_selectedSet, a, true);
            BuildBindings(*detail, m_selectedSet, a);
            BuildProcessing(*detail, m_selectedSet, a);
        }
        m_detailScroll->RemoveAllViews();
        {
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            m_detailScroll->AddView(detail.Get(), match);
        }
        ui::LayoutStyle fill;
        fill.FlexGrow = 1.0f;
        fill.Height = ui::SizeSpec::Match();
        m_body->AddView(m_detailScroll.Get(), fill);
    }

    // === The editors ==========================================================================

    void InputMapEditorPage::BuildSetFields(ui::FlexLayout& column, usize s)
    {
        const input::ActionSet& set = m_map.sets[s];
        InputMapEditorPage* self = this;
        AddHeading(column, u8"Action set");
        {
            auto row = Row();
            AddLabel(*row, u8"Name", 12.0f, true, kLabelWidth);
            AddNameField(*row, set.name.AsView(),
                         [self, s](StringView value)
                         {
                             const String renamed(value);
                             self->Mutate(
                                 [s, renamed](input::InputMap& m)
                                 {
                                     if (input::ActionSet* target = input_map_edit::SetAt(m, s))
                                     {
                                         target->name = renamed;
                                     }
                                 });
                         });
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column.AddView(row.Get(), match);
        }
        {
            auto row = Row();
            AddNumber(*row, u8"Priority", u8"When two active sets bind the same input, the higher priority takes it",
                      static_cast<f64>(set.priority), -100.0, 100.0, 0, Format(u8"priority/{}", s).AsView(),
                      [s](input::InputMap& m, f64 v)
                      {
                          if (input::ActionSet* target = input_map_edit::SetAt(m, s))
                          {
                              target->priority = static_cast<i32>(v);
                          }
                      });
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column.AddView(row.Get(), match);
        }
        AddNote(column, u8"A game turns sets on and off together (gameplay, a menu). When two active sets "
                        u8"bind the same input, the one with the higher priority takes it.");
        AddNote(column, Format(u8"{} actions: pick one on the left to edit it.", set.actions.Size()).AsView());
    }

    void InputMapEditorPage::BuildActionHeader(ui::FlexLayout& column, usize s, usize a, bool withName)
    {
        const input::Action& action = m_map.sets[s].actions[a];
        InputMapEditorPage* self = this;
        auto row = Row(8.0f);
        if (withName)
        {
            ui::EditText* name = AddNameField(*row, action.name.AsView(),
                                              [self, s, a](StringView value)
                                              {
                                                  const String renamed(value);
                                                  self->MutateAction(s, a, [renamed](input::Action& x)
                                                                     { x.name = renamed; });
                                              },
                                              180.0f);
            name->SetStyle(ui::StyleProperty::FontSize, 14.0f);
        }
        auto spacer = MakeRef<ui::View>(Allocator());
        ui::LayoutStyle grow;
        grow.FlexGrow = 1.0f;
        row->AddView(spacer.Get(), grow);

        const StringView kinds[] = {input_map_edit::KindTitle(input::ActionKind::Button),
                                    input_map_edit::KindTitle(input::ActionKind::Axis1D),
                                    input_map_edit::KindTitle(input::ActionKind::Axis2D)};
        ui::ComboBox* kind = AddChoice(*row, Span<const StringView>(kinds, 3), static_cast<i32>(action.kind), 100.0f,
                                       [s, a](input::InputMap& m, i32 index)
                                       {
                                           if (input::Action* x = input_map_edit::ActionAt(m, s, a))
                                           {
                                               x->kind = static_cast<input::ActionKind>(index);
                                           }
                                       });
        kind->TooltipText = String(u8"What the action reports: pressed or not, one axis, or two");
        const StringView interactions[] = {u8"On press", u8"Hold", u8"Tap", u8"Double tap"};
        ui::ComboBox* interaction =
            AddChoice(*row, Span<const StringView>(interactions, 4), static_cast<i32>(action.interaction.kind), 110.0f,
                      [s, a](input::InputMap& m, i32 index)
                      {
                          if (input::Action* x = input_map_edit::ActionAt(m, s, a))
                          {
                              x->interaction.kind = static_cast<input::InteractionKind>(index);
                          }
                      });
        interaction->TooltipText = String(u8"When it fires: as soon as pressed, after a hold, on a quick tap, or on two");
        if (action.interaction.kind != input::InteractionKind::None)
        {
            AddNumber(*row, SecondsLabel(action.interaction.kind), u8"Seconds", action.interaction.seconds, 0.0,
                      10.0, 2, Format(u8"seconds/{}/{}", s, a).AsView(),
                      [s, a](input::InputMap& m, f64 v)
                      {
                          if (input::Action* x = input_map_edit::ActionAt(m, s, a))
                          {
                              x->interaction.seconds = static_cast<f32>(v);
                          }
                      });
            AddLabel(*row, u8"s", 12.0f, true);
        }
        AddIcon(*row, app::EditorIcons::Get().remove.Get(), u8"Remove the action",
                [self, s, a]()
                {
                    self->Mutate(
                        [s, a](input::InputMap& m)
                        {
                            if (input::ActionSet* set = input_map_edit::SetAt(m, s); set != nullptr && a < set->actions.Size())
                            {
                                set->actions.RemoveAt(a);
                            }
                        });
                });
        ui::LayoutStyle match;
        match.Width = ui::SizeSpec::Match();
        column.AddView(row.Get(), match);
    }

    void InputMapEditorPage::BuildBindings(ui::FlexLayout& column, usize s, usize a)
    {
        const input::Action& action = m_map.sets[s].actions[a];
        InputMapEditorPage* self = this;
        {
            auto head = Row(8.0f);
            AddLabel(*head, u8"Bindings", 12.0f, true);
            auto spacer = MakeRef<ui::View>(Allocator());
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            head->AddView(spacer.Get(), grow);
            auto add = MakeRef<ui::Button>(Allocator(), StringView(u8"Add Binding..."));
            add->FontSize.SetValue(Optional<f32>(11.0f));
            add->TooltipText = String(u8"Bind another input: the sources this kind of action takes");
            ui::Button* raw = add.Get();
            add->OnClick.Add([self, raw, s, a](ui::ButtonBase*) { self->ShowAddBindingMenu(*raw, s, a); });
            ui::LayoutStyle centred;
            centred.AlignSelf = ui::Align::Center;
            head->AddView(add.Get(), centred);
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column.AddView(head.Get(), match);
        }
        if (action.bindings.IsEmpty())
        {
            AddNote(column, u8"No bindings: nothing triggers this action yet.");
        }
        for (usize b = 0; b < action.bindings.Size(); ++b)
        {
            BuildBindingCard(column, s, a, b);
        }
    }

    void InputMapEditorPage::ShowAddBindingMenu(ui::View& anchor, usize s, usize a)
    {
        const input::Action* action = input_map_edit::ActionAt(m_map, s, a);
        if (action == nullptr || anchor.Context == nullptr)
        {
            return;
        }
        input::BindingSource valid[8];
        const usize n = input::ValidSources(action->kind, valid);
        auto menu = MakeRef<ui::ContextMenu>(Allocator());
        InputMapEditorPage* self = this;
        for (usize i = 0; i < n; ++i)
        {
            const input::BindingSource source = valid[i];
            menu->AddItem(input_map_edit::SourceTitle(source), [self, s, a, source]() { self->AddBinding(s, a, source); });
        }
        const Float2 at = anchor.LocalToScreen(Float2{0.0f, anchor.Height()});
        menu->Show(anchor.Context, at.x, at.y);
    }

    void InputMapEditorPage::BuildBindingCard(ui::FlexLayout& column, usize s, usize a, usize b)
    {
        const input::Action& action = m_map.sets[s].actions[a];
        const input::Binding& binding = action.bindings[b];
        InputMapEditorPage* self = this;
        auto inner = Column(6.0f);
        inner->Padding = ui::Thickness{10, 8};

        auto top = Row(8.0f);
        AddLabel(*top, input_map_edit::SourceTitle(binding.source), 11.5f, true, 110.0f);
        if (binding.source == input::BindingSource::Composite2D)
        {
            // The four keys as they sit on a keyboard: up over left, down, right.
            auto cross = Column(3.0f);
            auto upper = Row(3.0f);
            auto lower = Row(3.0f);
            auto gap = MakeRef<ui::View>(Allocator());
            ui::LayoutStyle cell;
            cell.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(46.0f));
            upper->AddView(gap.Get(), cell);
            AddKeycap(*upper, detail::KeyName(binding.posY).AsView(), s, a, b, 3, 46.0f);
            AddKeycap(*lower, detail::KeyName(binding.negX).AsView(), s, a, b, 0, 46.0f);
            AddKeycap(*lower, detail::KeyName(binding.negY).AsView(), s, a, b, 2, 46.0f);
            AddKeycap(*lower, detail::KeyName(binding.posX).AsView(), s, a, b, 1, 46.0f);
            cross->AddView(upper.Get());
            cross->AddView(lower.Get());
            top->AddView(cross.Get());
        }
        else
        {
            AddKeycap(*top, detail::DescribeBinding(binding).AsView(), s, a, b, -1, 96.0f);
        }
        auto spacer = MakeRef<ui::View>(Allocator());
        ui::LayoutStyle grow;
        grow.FlexGrow = 1.0f;
        top->AddView(spacer.Get(), grow);
        AddIcon(*top, app::EditorIcons::Get().remove.Get(), u8"Remove the binding",
                [self, s, a, b]()
                {
                    self->Mutate(
                        [s, a, b](input::InputMap& m)
                        {
                            if (input::Action* x = input_map_edit::ActionAt(m, s, a); x != nullptr && b < x->bindings.Size())
                            {
                                x->bindings.RemoveAt(b);
                            }
                        });
                });
        ui::LayoutStyle match;
        match.Width = ui::SizeSpec::Match();
        inner->AddView(top.Get(), match);

        if (!SourceFitsKind(action.kind, binding.source))
        {
            auto warning = MakeRef<ui::Label>(
                Allocator(), Format(u8"A {} does not take a {} binding: remove it, or change the action's kind.",
                                    input_map_edit::KindTitle(action.kind), input_map_edit::SourceTitle(binding.source))
                                 .AsView());
            warning->FontSize.SetValue(11.0f);
            warning->WordWrap.SetValue(true);
            warning->TextColor.SetValue(Optional<Color>(kWarningText));
            inner->AddView(warning.Get(), match);
        }
        BuildBindingSettings(*inner, s, a, b);
        column.AddView(Card(*inner, 0.05f).Get(), match);
    }

    void InputMapEditorPage::BuildBindingSettings(ui::FlexLayout& column, usize s, usize a, usize b)
    {
        using Source = input::BindingSource;
        const input::Binding& binding = m_map.sets[s].actions[a].bindings[b];
        const Source source = binding.source;
        const bool hasDeadZone = source == Source::MouseAxis || source == Source::GamepadAxis ||
                                 source == Source::GamepadStick || source == Source::TouchStick;
        const bool hasScale = source != Source::TouchButton;
        const bool hasInvert = source == Source::MouseAxis || source == Source::MouseDelta ||
                               source == Source::GamepadAxis || source == Source::GamepadStick ||
                               source == Source::TouchStick || source == Source::Composite2D;
        const bool hasDevice =
            source == Source::GamepadButton || source == Source::GamepadAxis || source == Source::GamepadStick;
        const bool hasRegion = source == Source::TouchButton || source == Source::TouchStick;
        const auto onBinding = [s, a, b](auto apply)
        {
            return [s, a, b, apply](input::InputMap& m, auto v)
            {
                if (input::Action* x = input_map_edit::ActionAt(m, s, a); x != nullptr && b < x->bindings.Size())
                {
                    apply(x->bindings[b], v);
                }
            };
        };
        const auto key = [s, a, b](StringView field) { return Format(u8"{}/{}/{}/{}", field, s, a, b); };

        auto row = Row(14.0f);
        row->Padding = ui::Thickness{118, 0, 0, 0}; // under the input, past the source's name
        if (hasDeadZone)
        {
            AddNumber(*row, u8"Dead zone", u8"Movement smaller than this reads as none", binding.deadZone, 0.0, 1.0, 2,
                      key(u8"deadZone").AsView(),
                      onBinding([](input::Binding& x, f64 v) { x.deadZone = static_cast<f32>(v); }));
        }
        if (hasScale)
        {
            AddNumber(*row, u8"Scale", u8"Multiplies what the input reports (negative flips it)", binding.scale,
                      -100.0, 100.0, 2, key(u8"scale").AsView(),
                      onBinding([](input::Binding& x, f64 v) { x.scale = static_cast<f32>(v); }));
        }
        if (hasInvert)
        {
            AddCheck(*row, u8"Invert", u8"Flip the direction", binding.invert,
                     onBinding([](input::Binding& x, bool v) { x.invert = v; }));
        }
        if (source == Source::Composite2D)
        {
            AddCheck(*row, u8"Normalize", u8"Diagonals as long as straight moves", binding.normalize,
                     onBinding([](input::Binding& x, bool v) { x.normalize = v; }));
        }
        if (hasDevice)
        {
            AddLabel(*row, u8"Gamepad", 11.5f, true);
            const StringView pads[] = {u8"Any", u8"1", u8"2", u8"3", u8"4"};
            ui::ComboBox* pad = AddChoice(*row, Span<const StringView>(pads, 5), Clamp(binding.device + 1, 0, 4), 70.0f,
                                          onBinding([](input::Binding& x, i32 index) { x.device = index - 1; }));
            pad->TooltipText = String(u8"Which connected gamepad: any, or one by its number");
        }
        if (row->ChildCount() > 0)
        {
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column.AddView(row.Get(), match);
        }
        if (hasRegion)
        {
            auto region = Row(14.0f);
            region->Padding = ui::Thickness{118, 0, 0, 0};
            AddLabel(*region, u8"Screen area", 11.5f, true);
            const char8_t* names[] = {u8"X", u8"Y", u8"Width", u8"Height"};
            const f32 values[] = {binding.regionX, binding.regionY, binding.regionW, binding.regionH};
            for (u32 i = 0; i < 4; ++i)
            {
                AddNumber(*region, names[i], u8"A fraction of the screen, 0 to 1", values[i], 0.0, 1.0, 2,
                          key(names[i]).AsView(),
                          onBinding(
                              [i](input::Binding& x, f64 v)
                              {
                                  f32* fields[] = {&x.regionX, &x.regionY, &x.regionW, &x.regionH};
                                  *fields[i] = static_cast<f32>(v);
                              }));
            }
            if (source == Source::TouchStick)
            {
                AddNumber(*region, u8"Radius", u8"How far the stick reaches, in pixels", binding.stickRadius, 0.0,
                          1000.0, 0, key(u8"radius").AsView(),
                          onBinding([](input::Binding& x, f64 v) { x.stickRadius = static_cast<f32>(v); }));
            }
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column.AddView(region.Get(), match);
        }
    }

    void InputMapEditorPage::BuildProcessing(ui::FlexLayout& column, usize s, usize a)
    {
        const input::Action& action = m_map.sets[s].actions[a];
        if (action.kind == input::ActionKind::Button)
        {
            return; // a button reports pressed or not: nothing to shape
        }
        {
            auto head = Row(8.0f); // at the left, as "Bindings" is (a label alone in a column centres)
            AddLabel(*head, u8"Processing", 12.0f, true);
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column.AddView(head.Get(), match);
        }
        const auto onAction = [s, a](auto apply)
        {
            return [s, a, apply](input::InputMap& m, auto v)
            {
                if (input::Action* x = input_map_edit::ActionAt(m, s, a))
                {
                    apply(x->processors, v);
                }
            };
        };
        const auto key = [s, a](StringView field) { return Format(u8"{}/{}/{}", field, s, a); };
        auto row = Row(14.0f);
        AddNumber(*row, u8"Sensitivity", u8"Above 0, a key-driven axis ramps toward its value at this rate a second",
                  action.processors.sensitivity, 0.0, 100.0, 2, key(u8"sensitivity").AsView(),
                  onAction([](input::ActionProcessors& p, f64 v) { p.sensitivity = static_cast<f32>(v); }));
        AddNumber(*row, u8"Gravity", u8"Above 0, the rate a second it returns to rest once released",
                  action.processors.gravity, 0.0, 100.0, 2, key(u8"gravity").AsView(),
                  onAction([](input::ActionProcessors& p, f64 v) { p.gravity = static_cast<f32>(v); }));
        AddNumber(*row, u8"Curve", u8"Shapes an analog input: the value to this power (1 is straight)",
                  action.processors.responseExponent, 0.1, 5.0, 2, key(u8"curve").AsView(),
                  onAction([](input::ActionProcessors& p, f64 v) { p.responseExponent = static_cast<f32>(v); }));
        ui::LayoutStyle match;
        match.Width = ui::SizeSpec::Match();
        column.AddView(row.Get(), match);
        auto flags = Row(14.0f);
        AddCheck(*flags, u8"Snap on reverse", u8"Jump to zero first when the direction flips", action.processors.snap,
                 onAction([](input::ActionProcessors& p, bool v) { p.snap = v; }));
        AddCheck(*flags, u8"Scale with time", u8"Multiply by the game's time scale, once there is one",
                 action.processors.timeScale, onAction([](input::ActionProcessors& p, bool v) { p.timeScale = v; }));
        column.AddView(flags.Get(), match);
    }

    // === Small pieces ===========================================================================

    RefPtr<ui::FlexLayout> InputMapEditorPage::Row(f32 spacing)
    {
        auto row = MakeRef<ui::FlexLayout>(Allocator());
        row->Direction = ui::Orientation::Horizontal;
        row->Spacing = spacing;
        row->AlignItems = ui::Align::Center;
        return row;
    }

    RefPtr<ui::FlexLayout> InputMapEditorPage::Column(f32 spacing)
    {
        auto column = MakeRef<ui::FlexLayout>(Allocator());
        column->Direction = ui::Orientation::Vertical;
        column->Spacing = spacing;
        return column;
    }

    ui::Label* InputMapEditorPage::AddLabel(ui::FlexLayout& row, StringView text, f32 size, bool dim, f32 width)
    {
        auto label = MakeRef<ui::Label>(Allocator(), text);
        label->FontSize.SetValue(size);
        if (dim)
        {
            label->TextColor.SetValue(Optional<Color>(kDimText));
        }
        ui::LayoutStyle lp;
        if (width > 0.0f)
        {
            lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(width));
        }
        lp.AlignSelf = ui::Align::Center;
        row.AddView(label.Get(), lp);
        return label.Get();
    }

    void InputMapEditorPage::AddHeading(ui::FlexLayout& column, StringView text)
    {
        auto label = MakeRef<ui::Label>(Allocator(), text);
        label->FontSize.SetValue(15.0f);
        column.AddView(label.Get());
    }

    void InputMapEditorPage::AddNote(ui::FlexLayout& column, StringView text)
    {
        auto note = MakeRef<ui::Label>(Allocator(), text);
        note->FontSize.SetValue(11.0f);
        note->WordWrap.SetValue(true);
        note->TextColor.SetValue(Optional<Color>(kDimText));
        ui::LayoutStyle match;
        match.Width = ui::SizeSpec::Match();
        column.AddView(note.Get(), match);
    }

    RefPtr<ui::Panel> InputMapEditorPage::Card(ui::View& content, f32 fill)
    {
        auto panel = MakeRef<ui::Panel>(Allocator());
        panel->SetStyle(ui::StyleProperty::Background,
                        Rounded(Allocator(), Color{1, 1, 1, fill}, 6.0f, Color{1, 1, 1, 0.09f}));
        panel->AddView(&content);
        return panel;
    }

    ui::EditText* InputMapEditorPage::AddNameField(ui::FlexLayout& row, StringView value,
                                                   Function<void(StringView)> commit, f32 width)
    {
        auto field = MakeRef<ui::EditText>(Allocator());
        field->SetText(value);
        field->OnCommit.Add(
            [fn = Move(commit)](ui::EditText* edit)
            {
                if (fn && !edit->Text().IsEmpty())
                {
                    fn(edit->Text());
                }
            });
        ui::LayoutStyle lp;
        if (width > 0.0f)
        {
            lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(width));
        }
        else
        {
            lp.FlexGrow = 1.0f;
        }
        lp.AlignSelf = ui::Align::Center;
        row.AddView(field.Get(), lp);
        return field.Get();
    }

    ui::NumericField* InputMapEditorPage::AddNumber(ui::FlexLayout& row, StringView label, StringView tooltip, f64 value,
                                                    f64 least, f64 most, u32 decimals, StringView field,
                                                    Function<void(input::InputMap&, f64)> apply)
    {
        auto pair = Row(5.0f);
        AddLabel(*pair, label, 11.5f, true);
        auto number = MakeRef<ui::NumericField>(Allocator());
        number->SetDecimalPlaces(static_cast<i32>(decimals));
        number->SetMin(least);
        number->SetMax(most);
        number->SetStep(decimals == 0 ? 1.0 : 0.05);
        number->SetValue(value);
        number->TooltipText = String(tooltip);
        InputMapEditorPage* self = this;
        const String key(field);
        number->OnValueChanged.Add(
            [self, key, fn = Move(apply)](ui::NumericField*, f64 v)
            { self->MutateField(key.AsView(), [&fn, v](input::InputMap& m) { fn(m, v); }); });
        ui::LayoutStyle fixed;
        fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(64.0f));
        fixed.AlignSelf = ui::Align::Center;
        pair->AddView(number.Get(), fixed);
        row.AddView(pair.Get());
        return number.Get();
    }

    ui::CheckBox* InputMapEditorPage::AddCheck(ui::FlexLayout& row, StringView label, StringView tooltip, bool value,
                                               Function<void(input::InputMap&, bool)> apply)
    {
        auto check = MakeRef<ui::CheckBox>(Allocator(), label, value);
        check->FontSize.SetValue(11.5f);
        check->TooltipText = String(tooltip);
        InputMapEditorPage* self = this;
        check->OnCheckedChanged.Add([self, fn = Move(apply)](ui::CheckBox*, bool v)
                                    { self->Mutate([&fn, v](input::InputMap& m) { fn(m, v); }); });
        ui::LayoutStyle centred;
        centred.AlignSelf = ui::Align::Center;
        row.AddView(check.Get(), centred);
        return check.Get();
    }

    ui::ComboBox* InputMapEditorPage::AddChoice(ui::FlexLayout& row, Span<const StringView> items, i32 selected,
                                                f32 width, Function<void(input::InputMap&, i32)> apply)
    {
        auto combo = MakeRef<ui::ComboBox>(Allocator());
        for (const StringView item : items)
        {
            (void)combo->AddItem(item);
        }
        combo->SetSelectedIndex(selected);
        InputMapEditorPage* self = this;
        combo->OnSelectionChanged.Add([self, fn = Move(apply)](ui::ComboBox*, i32 index)
                                      { self->Mutate([&fn, index](input::InputMap& m) { fn(m, index); }); });
        ui::LayoutStyle fixed;
        fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(width));
        fixed.AlignSelf = ui::Align::Center;
        row.AddView(combo.Get(), fixed);
        return combo.Get();
    }

    ui::Button* InputMapEditorPage::AddKeycap(ui::FlexLayout& row, StringView text, usize s, usize a, usize b,
                                              i32 direction, f32 minWidth)
    {
        const bool listening = m_listening && m_listenSet == s && m_listenAction == a && m_listenBinding == b &&
                               m_listenDirection == direction;
        auto cap = MakeRef<ui::Button>(Allocator(), listening ? StringView(u8"Press...") : text);
        cap->FontSize.SetValue(Optional<f32>(12.0f));
        cap->SetStyle(ui::StyleProperty::Padding, ui::Thickness{8.0f, 3.0f});
        cap->TooltipText = String(listening ? StringView(u8"Press the new input, or click to cancel")
                                            : StringView(u8"Click, then press the new input"));
        auto look = MakeRef<ui::StateListDrawable>(Allocator());
        const Color fill = listening ? Color{kAccent.r, kAccent.g, kAccent.b, 0.35f} : Color{1, 1, 1, 0.07f};
        const Color edge = listening ? kAccent : Color{1, 1, 1, 0.22f};
        look->Set(ui::ControlState::Normal, Rounded(Allocator(), fill, 4.0f, edge));
        look->Set(ui::ControlState::Hover,
                  Rounded(Allocator(), Color{fill.r, fill.g, fill.b, fill.a + 0.06f}, 4.0f, edge));
        look->Set(ui::ControlState::Pressed,
                  Rounded(Allocator(), Color{fill.r, fill.g, fill.b, fill.a + 0.10f}, 4.0f, edge));
        cap->SetStyle(ui::StyleProperty::Background, RefPtr<ui::Drawable>(look.Get()));
        InputMapEditorPage* self = this;
        cap->OnClick.Add([self, s, a, b, direction](ui::ButtonBase*) { self->BeginListen(s, a, b, direction); });
        ui::LayoutStyle lp;
        lp.AlignSelf = ui::Align::Center;
        if (minWidth > 0.0f)
        {
            lp.MinWidth = ui::Unit::Dp(minWidth);
        }
        row.AddView(cap.Get(), lp);
        return cap.Get();
    }

    ui::IconButton* InputMapEditorPage::AddIcon(ui::FlexLayout& row, ui::SVGDrawable* icon, StringView tooltip,
                                                Function<void()> onClick, f32 size)
    {
        auto button = MakeRef<ui::IconButton>(Allocator(), icon, size);
        button->TooltipText = String(tooltip);
        button->OnClick.Add(
            [fn = Move(onClick)](ui::ButtonBase*)
            {
                if (fn)
                {
                    fn();
                }
            });
        ui::LayoutStyle lp;
        lp.AlignSelf = ui::Align::Center;
        row.AddView(button.Get(), lp);
        return button.Get();
    }

    void InputMapEditorPage::RefreshStatus()
    {
        if (m_listening)
        {
            m_status->SetText(u8"Listening: press the new input (click the key cap again to cancel)");
            m_status->TextColor.SetValue(Optional<Color>(kAccent));
            return;
        }
        String error;
        if (!input::ValidateInputMap(m_map, &error))
        {
            m_status->SetText(Format(u8"Cannot save yet: {}", error.AsView()).AsView());
            m_status->TextColor.SetValue(Optional<Color>(kWarningText));
            return;
        }
        usize actions = 0;
        usize bindings = 0;
        for (const input::ActionSet& set : m_map.sets)
        {
            actions += set.actions.Size();
            for (const input::Action& action : set.actions)
            {
                bindings += action.bindings.Size();
            }
        }
        m_status->SetText(
            Format(u8"{} sets · {} actions · {} bindings", m_map.sets.Size(), actions, bindings).AsView());
        m_status->TextColor.SetValue(Optional<Color>(kDimText));
    }

    const TypeInfo* InputMapPageFactory::PrimaryType() const
    {
        return &pipeline::InputMapAsset::StaticType();
    }

    UniquePtr<EditorPage> InputMapPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<InputMapEditorPage>(context, *m_host, instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
