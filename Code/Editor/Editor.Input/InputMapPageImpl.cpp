// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Input - the input map page's implementation (see InputMapPage.cppm).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.input;

import foundation.core;
import foundation.content;
import foundation.shell;
import foundation.runtime.client;
import foundation.input;
import input.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace input = foundation::input;

namespace editor
{
    RTTI_DEFINE_OBJECT(InputCaptureEditor, "rtti::editor::input")

    InputMapEditorPage::InputMapEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::InputMapAsset::StaticType(), kPath), m_format(context.Allocator())
    {
        HideAssetRows(); // the map's rows are the chosen set's, beside the tree
        InputMapEditorPage* self = this;
        m_model = MakeUnique<gui::TreeModel>(Allocator());
        m_tree = Find<gui::TreeView>(u8"input-tree");
        if (auto* add = Find<gui::ButtonBase>(u8"input-add-set"))
            add->SetOnClick([self]() { self->AddSet(); });
        if (auto* remove = Find<gui::ButtonBase>(u8"input-remove"))
            remove->SetOnClick([self]() { (void)self->RemoveChosen(); });
        if (auto* search = Find<gui::TextField>(u8"input-search"))
            search->SetOnTextChanging([self](StringView text) { self->Search(text); });
        if (m_tree != nullptr)
        {
            m_tree->SetModel(m_model.Get());
            m_tree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                          {
                                              if (self->m_showing || !index.IsValid())
                                                  return;
                                              const usize node = static_cast<usize>(index.InternalId);
                                              if (node >= self->m_rows.Size())
                                                  return;
                                              const TreeRow row = self->m_rows[node];
                                              if (row.action >= 0)
                                                  self->ChooseAction(row.set, row.action);
                                              else
                                                  self->ChooseSet(row.set);
                                          });
        }

        // The page answers the capture editor's questions. The editor asks by the grid's path in
        // the set's section ("set/actions.0.bindings.0.code"); the page works in the set's own paths.
        m_capture.describe = [self](StringView path) -> String
        {
            usize action = 0, binding = 0;
            i32 direction = -1;
            if (!self->BindingOf(SetPath(path), action, binding, direction))
                return String{};
            const input::Binding& b = self->Map()->sets[static_cast<usize>(self->m_set)].actions[action].bindings[binding];
            if (direction < 0)
                return input::DescribeBinding(b);
            const u32 codes[] = {b.negX, b.posX, b.negY, b.posY};
            return codes[direction] != 0 ? String(input::KeyName(codes[direction])) : String(u8"Not set");
        };
        m_capture.toggle = [self](StringView path) { self->ToggleListen(SetPath(path)); };
        m_capture.listening = [self](StringView path) { return !self->m_listenPath.IsEmpty() && self->m_listenPath.AsView() == SetPath(path); };
        m_capture.cancel = [self]() { self->StopListen(); };
        if (Grid() != nullptr)
            InputCaptureEditor::Register(Grid()->GetEditors(), m_capture);

        // The chosen set's rows come from reflection, described in the page's words, and each edit
        // is one undo step.
        m_values = MakeUnique<gui::ReflectedPropertySource>(Allocator(), Allocator());
        m_values->SetTextFormat(&m_format);
        m_values->SetOnDescribe([self](StringView path, gui::PropertyInfo& info)
                                {
                                    const input::InputMap* map = self->Map();
                                    if (map != nullptr && self->m_set >= 0 && static_cast<usize>(self->m_set) < map->sets.Size())
                                        DescribeInputRow(map->sets[static_cast<usize>(self->m_set)], path, info);
                                });
        m_values->SetOnChange([self](const gui::ReflectedChange& change)
                              {
                                  const i32 set = self->m_set;
                                  (void)self->Commands().Execute(UniquePtr<IEditorCommand>(
                                      EditorRootAllocator().New<app::ReflectedEditCommand>(
                                          TypeOf<input::ActionSet>(),
                                          [self, set]() -> void*
                                          {
                                              input::InputMap* map = self->Map();
                                              return map != nullptr && set >= 0 && static_cast<usize>(set) < map->sets.Size() ? &map->sets[static_cast<usize>(set)] : nullptr;
                                          },
                                          change, self->m_format, u8"edit_input",
                                          [self]()
                                          {
                                              // After an edit (a name, a kind, a binding): the rows' words, the tree, the map's verdict.
                                              self->m_values->Refresh();
                                              self->ShowTree();
                                              self->ShowValidity();
                                          }),
                                      EditorRootAllocator()));
                              });
        AddSection(kSetSection, u8"Set", *m_values);
        const input::InputMap* map = Map();
        ChooseSet(map != nullptr && !map->sets.IsEmpty() ? 0 : -1);
        ShowValidity();
    }

    StringView InputMapEditorPage::SetPath(StringView gridPath)
    {
        const usize skip = kSetSection.Size() + 1; // "set/"
        return gridPath.StartsWith(kSetSection) && gridPath.Size() >= skip && gridPath[skip - 1] == u8'/' ? gridPath.SubStr(skip, gridPath.Size() - skip)
                                                                                                         : gridPath;
    }

    InputMapEditorPage::~InputMapEditorPage()
    {
        if (m_tree != nullptr)
            m_tree->SetModel(nullptr);
    }

    input::InputMap* InputMapEditorPage::Map() const
    {
        auto* asset = ObjectAs<pipeline::InputMapAsset>();
        return asset != nullptr ? &asset->Map() : nullptr;
    }

    gui::IPropertySource& InputMapEditorPage::SetRows() noexcept
    {
        return *m_values;
    }

    String InputMapEditorPage::Tree() const
    {
        String out;
        const input::InputMap* map = Map();
        for (const TreeRow& row : m_rows)
        {
            if (!out.IsEmpty())
                out += u8"\n";
            const input::ActionSet& set = map->sets[static_cast<usize>(row.set)];
            out += row.action < 0 ? set.name.AsView() : Format(u8"  {}", set.actions[static_cast<usize>(row.action)].name).AsView();
        }
        return out;
    }

    void InputMapEditorPage::BindSet()
    {
        input::InputMap* map = Map();
        if (map == nullptr || m_set < 0 || static_cast<usize>(m_set) >= map->sets.Size())
        {
            m_values->SetObjects(Span<const Instance>{});
            return;
        }
        // The set's address is read again each time, because the sets' array moves as sets are
        // added and removed.
        const Instance set(&map->sets[static_cast<usize>(m_set)], &TypeOf<input::ActionSet>());
        m_values->SetObjects(Span<const Instance>(&set, 1u));
    }

    void InputMapEditorPage::ChooseSet(i32 set)
    {
        StopListen();
        m_set = set;
        m_chosen = TreeRow{set, -1};
        BindSet();
        ShowTree();
    }

    void InputMapEditorPage::ChooseAction(i32 set, i32 action)
    {
        if (set != m_set)
            ChooseSet(set);
        m_chosen = TreeRow{set, action};
        // Open in place: its list, then the action, brought into view.
        if (gui::PropertyGrid* grid = Grid())
        {
            grid->SetOpen(Sections().PathIn(kSetSection, u8"actions").AsView(), true);
            const String path = Sections().PathIn(kSetSection, Format(u8"actions.{}", action).AsView());
            grid->SetOpen(path.AsView(), true);
            const i32 row = grid->FindRow(path.AsView());
            if (row >= 0)
                grid->ScrollToItem(row, gui::ScrollAlign::Start);
        }
        ShowTree();
    }

    void InputMapEditorPage::ShowTree()
    {
        const input::InputMap* map = Map();
        if (map == nullptr)
            return;
        m_showing = true;
        // A set the user folded stays folded through the rebuild (a choice, an edit, a search).
        Array<i32> folded(Allocator());
        if (m_tree != nullptr)
            for (usize node = 0; node < m_rows.Size(); ++node)
                if (m_rows[node].action < 0 && !m_tree->IsIdExpanded(static_cast<gui::ItemId>(node)))
                    folded.PushBack(m_rows[node].set);
        m_model->Clear();
        m_rows.Clear();
        i32 chosen = -1;
        for (usize s = 0; s < map->sets.Size(); ++s)
        {
            const input::ActionSet& set = map->sets[s];
            // A search keeps the actions whose name or input holds it, and the sets holding one.
            Array<usize> shown;
            for (usize a = 0; a < set.actions.Size(); ++a)
            {
                const input::Action& action = set.actions[a];
                bool matches = m_search.IsEmpty() || action.name.AsView().ContainsIgnoreCase(m_search.AsView());
                for (const input::Binding& binding : action.bindings)
                    matches = matches || input::DescribeBinding(binding).AsView().ContainsIgnoreCase(m_search.AsView());
                if (matches)
                    shown.PushBack(a);
            }
            if (!m_search.IsEmpty() && shown.IsEmpty() && !set.name.AsView().ContainsIgnoreCase(m_search.AsView()))
                continue;
            const i32 setNode = m_model->AddNode(gui::kTreeModelRoot, set.name.AsView());
            m_model->SetIcon(setNode, u8"Editor/Icons/Types/InputMapAsset.svg", u8"cat-scripting");
            m_rows.PushBack(TreeRow{static_cast<i32>(s), -1});
            if (m_chosen.set == static_cast<i32>(s) && m_chosen.action < 0)
                chosen = setNode;
            for (const usize a : shown)
            {
                const input::Action& action = set.actions[a];
                const i32 node = m_model->AddNode(setNode, Format(u8"{}   {}", action.name, input_map_edit::KindTitle(action.kind)).AsView());
                m_model->SetIcon(node, action.kind == input::ActionKind::Button ? StringView(u8"Editor/Icons/play.svg") : StringView(u8"Editor/Icons/move.svg"));
                m_rows.PushBack(TreeRow{static_cast<i32>(s), static_cast<i32>(a)});
                if (m_chosen.set == static_cast<i32>(s) && m_chosen.action == static_cast<i32>(a))
                    chosen = node;
            }
        }
        if (m_tree != nullptr)
        {
            for (usize node = 0; node < m_rows.Size(); ++node)
                if (m_rows[node].action < 0)
                {
                    bool wasFolded = false;
                    for (const i32 set : folded)
                        wasFolded = wasFolded || set == m_rows[node].set;
                    m_tree->SetIdExpanded(static_cast<gui::ItemId>(node), !wasFolded);
                }
            if (chosen >= 0)
            {
                const gui::ItemId id = static_cast<gui::ItemId>(chosen);
                m_tree->SetSelectedIds(Span<const gui::ItemId>(&id, 1u));
            }
        }
        m_showing = false;
    }

    void InputMapEditorPage::ShowValidity()
    {
        const input::InputMap* map = Map();
        String error;
        const bool valid = map == nullptr || input::ValidateInputMap(*map, &error);
        SetNotice(u8"invalid", valid ? StringView{} : Format(u8"The game cannot read this map yet: {}", error).AsView());
        // What it holds, summed up in a line.
        if (map != nullptr)
        {
            usize actions = 0;
            usize bindings = 0;
            for (const input::ActionSet& set : map->sets)
                for (const input::Action& action : set.actions)
                {
                    ++actions;
                    bindings += action.bindings.Size();
                }
            const auto counted = [](usize n, StringView one, StringView many) { return Format(u8"{} {}", n, n == 1 ? one : many); };
            SetFact(u8"Holds", Format(u8"{}, {}, {}", counted(map->sets.Size(), u8"set", u8"sets"), counted(actions, u8"action", u8"actions"),
                                      counted(bindings, u8"binding", u8"bindings"))
                                   .AsView());
        }
    }

    void InputMapEditorPage::OnObjectChanged()
    {
        // A whole edit, or its undo: the set may be gone, or its address may have moved.
        const input::InputMap* map = Map();
        if (map != nullptr && (m_set < 0 || static_cast<usize>(m_set) >= map->sets.Size()))
            m_set = map->sets.IsEmpty() ? -1 : static_cast<i32>(map->sets.Size()) - 1;
        if (m_chosen.set != m_set || (m_chosen.action >= 0 && map != nullptr && static_cast<usize>(m_chosen.action) >= map->sets[static_cast<usize>(m_set)].actions.Size()))
            m_chosen = TreeRow{m_set, -1};
        BindSet();
        ShowTree();
        ShowValidity();
    }

    void InputMapEditorPage::AddSet()
    {
        input::InputMap* map = Map();
        if (map == nullptr)
            return;
        String name;
        for (usize n = 1;; ++n)
        {
            name = Format(u8"Set {}", n);
            bool taken = false;
            for (const input::ActionSet& set : map->sets)
                taken = taken || set.name.AsView() == name.AsView();
            if (!taken)
                break;
        }
        EditWhole(u8"edit_input", [name](ISerializable& object)
                  {
                      input::ActionSet set;
                      set.name = String(name.AsView());
                      static_cast<pipeline::InputMapAsset&>(object).Map().sets.PushBack(Move(set));
                  });
        ChooseSet(static_cast<i32>(Map()->sets.Size()) - 1);
    }

    bool InputMapEditorPage::RemoveChosen()
    {
        input::InputMap* map = Map();
        if (map == nullptr || m_chosen.set < 0 || static_cast<usize>(m_chosen.set) >= map->sets.Size())
            return false;
        const TreeRow row = m_chosen;
        StopListen();
        m_chosen = TreeRow{row.set, -1};
        EditWhole(u8"edit_input", [row](ISerializable& object)
                  {
                      input::InputMap& m = static_cast<pipeline::InputMapAsset&>(object).Map();
                      if (row.action < 0)
                          m.sets.RemoveAt(static_cast<usize>(row.set));
                      else if (static_cast<usize>(row.action) < m.sets[static_cast<usize>(row.set)].actions.Size())
                          m.sets[static_cast<usize>(row.set)].actions.RemoveAt(static_cast<usize>(row.action));
                  });
        return true;
    }

    void InputMapEditorPage::Search(StringView text)
    {
        m_search = String(Trim(text));
        ShowTree();
    }

    bool InputMapEditorPage::BindingOf(StringView path, usize& action, usize& binding, i32& direction) const
    {
        const input::InputMap* map = Map();
        if (map == nullptr || m_set < 0 || static_cast<usize>(m_set) >= map->sets.Size())
            return false;
        const input::ActionSet& set = map->sets[static_cast<usize>(m_set)];
        StringView rest;
        const i64 a = input_map_rows::IndexAfter(path, u8"actions.", rest);
        StringView field;
        const i64 b = a >= 0 ? input_map_rows::IndexAfter(rest, u8"bindings.", field) : -1;
        if (a < 0 || b < 0 || static_cast<usize>(a) >= set.actions.Size() || static_cast<usize>(b) >= set.actions[static_cast<usize>(a)].bindings.Size())
            return false;
        action = static_cast<usize>(a);
        binding = static_cast<usize>(b);
        direction = field == u8"negX" ? 0 : field == u8"posX" ? 1 : field == u8"negY" ? 2 : field == u8"posY" ? 3 : -1;
        return true;
    }

    void InputMapEditorPage::ToggleListen(StringView path)
    {
        if (m_listenPath.AsView() == path)
        {
            StopListen();
            return;
        }
        usize action = 0, binding = 0;
        i32 direction = -1;
        if (!BindingOf(path, action, binding, direction))
            return;
        m_listenPath = String(path);
        // What the action's kind takes (one key for a four-key binding's direction).
        m_listenFilter = input_map_edit::FilterFor(Map()->sets[static_cast<usize>(m_set)].actions[action].kind, direction >= 0);
        Context().SetStatus(u8"Listening: press the input to bind (click its button again to stop)");
        // The row's editor shows the listen itself (a refresh here could take its focus).
    }

    void InputMapEditorPage::StopListen()
    {
        if (m_listenPath.IsEmpty())
            return;
        m_listenPath.Clear();
        // Refresh the values: the rows stay and their editors are shown again (including the one
        // that was listening).
        if (Grid() != nullptr)
            Grid()->OnValuesChanged();
    }

    void InputMapEditorPage::LandCapture(const input::Binding& captured)
    {
        usize action = 0, binding = 0;
        i32 direction = -1;
        const bool found = BindingOf(m_listenPath.AsView(), action, binding, direction);
        const i32 set = m_set;
        StopListen();
        if (!found)
            return;
        EditWhole(u8"edit_input", [set, action, binding, direction, captured](ISerializable& object)
                  { input_map_edit::ApplyCapture(static_cast<pipeline::InputMapAsset&>(object).Map(), static_cast<usize>(set), action, binding, direction, captured); });
        Context().SetStatus(Format(u8"Bound {}", input::DescribeBinding(captured)).AsView());
    }

    void InputMapEditorPage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        if (m_listenPath.IsEmpty())
            return;
        // The devices are read as the game reads them (Escape is a key a game can bind, so it does
        // not cancel).
        auto* shellInput = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
        if (shellInput == nullptr)
            return;
        input::ShellInputSource devices(shellInput);
        input::Binding captured;
        if (input::CaptureBinding(devices, m_listenFilter, captured))
            LandCapture(captured);
    }

    Status InputMapEditorPage::Save()
    {
        const input::InputMap* map = Map();
        String error;
        if (map != nullptr && !input::ValidateInputMap(*map, &error))
        {
            Context().SetStatus(Format(u8"Not saved: the game cannot read this map yet: {}", error).AsView());
            return Status{ErrorCode::InvalidArgument};
        }
        return app::AssetObjectPage::Save();
    }

    const TypeInfo* InputMapPageFactory::PrimaryType() const
    {
        return &pipeline::InputMapAsset::StaticType();
    }

    UniquePtr<EditorPage> InputMapPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<InputMapEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
