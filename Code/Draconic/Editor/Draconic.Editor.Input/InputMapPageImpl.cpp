// Draconic::EditorInput - the `draconic.editor.input` module.
//
// InputMapPage (input P2): the editing surface for InputMapAsset - a scrollable
// sets > actions > bindings outline with add/remove, in-place renames, kind/interaction
// cycling, priority nudges, and "Listen" rebind capture (CaptureBinding polled per frame,
// filtered by the action's kind; Esc cancels). Every mutation is one UNDOABLE command via
// whole-map snapshots (the map is small data - the generic SnapshotCommand idea, page-local).
// Save validates first: a kind-mismatched map never reaches the cook.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include "Draconic.Foundation/Log/Log.h"
#include <cstdlib>

module draconic.editor.input;

import draconic.foundation;
import draconic.content;
import draconic.shell;
import draconic.runtime;
import draconic.runtime.client;
import draconic.input;
import draconic.input.editor;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.runtime;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

namespace draconic::editor
{
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
        draconic::content::Instance* instance =
            (m_context->Project() != nullptr)
                ? m_context->Project()->SourceDb().GetInstance(InstanceId())
                : nullptr;
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        input::InputMapAsset asset;
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
        if (!m_listening)
        {
            return;
        }
        auto* shellInput = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
        if (shellInput == nullptr)
        {
            return;
        }
        if (shellInput->Keyboard() != nullptr &&
            shellInput->Keyboard()->IsKeyPressed(draconic::shell::KeyCode::Escape))
        {
            m_listening = false;
            RefreshStatus();
            return;
        }
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
            Mutate(
                [set, action, binding, direction, captured](input::InputMap& map)
                {
                    if (set >= map.sets.Size())
                    {
                        return;
                    }
                    if (action >= map.sets[set].actions.Size())
                    {
                        return;
                    }
                    auto& bindings = map.sets[set].actions[action].bindings;
                    if (binding >= bindings.Size())
                    {
                        return;
                    }
                    if (direction < 0)
                    {
                        bindings[binding] = captured;
                        return;
                    }
                    // Composite direction: swap in just the captured KEY code.
                    u32* slot = direction == 0   ? &bindings[binding].negX
                                : direction == 1 ? &bindings[binding].posX
                                : direction == 2 ? &bindings[binding].negY
                                                 : &bindings[binding].posY;
                    *slot = captured.code;
                });
        }
    }

    ui::Button* InputMapEditorPage::MakeButton(ui::FlexLayout& row, StringView label, f32 width,
                                               Function<void()> onClick)
    {
        auto button = MakeRef<ui::Button>(DefaultAllocator(), label);
        button->FontSize.SetValue(Optional<f32>{11.0f});
        button->OnClick.Add(
            [fn = Move(onClick)](ui::ButtonBase*)
            {
                if (fn)
                {
                    fn();
                }
            });
        auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        lp->Width = ui::SizeSpec::Fixed(ui::Unit::Px(width));
        lp->Height = ui::SizeSpec::Match();
        row.AddView(button.Get(), lp);
        return button.Get();
    }

    RefPtr<ui::FlexLayout> InputMapEditorPage::MakeRow(f32 indent, f32 height)
    {
        auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
        row->Direction = ui::Orientation::Horizontal;
        row->Spacing = 4.0f;
        row->Padding = ui::Thickness{indent, 0};
        auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        lp->Width = ui::SizeSpec::Match();
        lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(height));
        m_rows->AddView(row.Get(), lp);
        return row;
    }

    void InputMapEditorPage::AddLabel(ui::FlexLayout& row, StringView text, f32 grow, f32 width)
    {
        auto label = MakeRef<ui::Label>(DefaultAllocator(), text);
        label->FontSize.SetValue(12.0f);
        auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        if (grow > 0.0f)
        {
            lp->Grow = grow;
        }
        else if (width > 0.0f)
        {
            lp->Width = ui::SizeSpec::Fixed(ui::Unit::Px(width));
        }
        lp->Height = ui::SizeSpec::Match();
        row.AddView(label.Get(), lp);
    }

    void InputMapEditorPage::AddFloatField(ui::FlexLayout& row, StringView label, f32 value,
                                           Function<void(f32)> commit, f32 width)
    {
        AddLabel(row, label, 0.0f, static_cast<f32>(label.Size()) * 7.0f + 6.0f);
        auto field = MakeRef<ui::EditableLabel>(DefaultAllocator());
        String text;
        AppendValue(text, value);
        field->SetText(text.AsView());
        field->FontSize.SetValue(12.0f);
        field->OnRenameCommitted.Add(
            [fn = Move(commit)](ui::EditableLabel*, StringView committed)
            {
                if (!fn || committed.IsEmpty())
                {
                    return;
                }
                String buffer(committed);
                char* end = nullptr;
                const f32 parsed = std::strtof(reinterpret_cast<const char*>(buffer.CStr()), &end);
                if (end != reinterpret_cast<const char*>(buffer.CStr()))
                {
                    fn(parsed);
                }
            });
        auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        lp->Width = ui::SizeSpec::Fixed(ui::Unit::Px(width));
        lp->Height = ui::SizeSpec::Match();
        row.AddView(field.Get(), lp);
    }

    void InputMapEditorPage::AddToggle(ui::FlexLayout& row, StringView label, bool value,
                                       Function<void(bool)> commit)
    {
        String text(label);
        text += value ? StringView(u8":on") : StringView(u8":off");
        MakeButton(row, text.AsView(), static_cast<f32>(text.Size()) * 7.0f + 14.0f,
                   [fn = Move(commit), value]()
                   {
                       if (fn)
                       {
                           fn(!value);
                       }
                   });
    }

    void InputMapEditorPage::AddNameEditor(ui::FlexLayout& row, StringView name,
                                           Function<void(StringView)> commit)
    {
        auto label = MakeRef<ui::EditableLabel>(DefaultAllocator());
        label->SetText(name);
        label->FontSize.SetValue(12.0f);
        label->OnRenameCommitted.Add(
            [fn = Move(commit)](ui::EditableLabel*, StringView value)
            {
                if (fn && !value.IsEmpty())
                {
                    fn(value);
                }
            });
        auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        lp->Grow = 1.0f;
        lp->Height = ui::SizeSpec::Match();
        row.AddView(label.Get(), lp);
    }

    void InputMapEditorPage::RequestRebuild()
    {
        ui::UIContext* ctx = (m_rows.Get() != nullptr) ? m_rows->Context : nullptr;
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
        m_rows->RemoveAllViews();
        InputMapEditorPage* self = this;

        for (usize s = 0; s < m_map.sets.Size(); ++s)
        {
            const input::ActionSet& set = m_map.sets[s];
            auto header = MakeRow(0.0f, 26.0f);
            AddNameEditor(*header, set.name.AsView(),
                          [self, s](StringView value)
                          {
                              String name(value);
                              self->Mutate(
                                  [s, name](input::InputMap& m)
                                  {
                                      if (s < m.sets.Size())
                                      {
                                          m.sets[s].name = name;
                                      }
                                  });
                          });
            String priority(u8"prio ");
            AppendValue(priority, static_cast<i64>(set.priority));
            AddLabel(*header, priority.AsView(), 0.0f, 52.0f);
            MakeButton(*header, u8"+", 22.0f,
                       [self, s]()
                       {
                           self->Mutate(
                               [s](input::InputMap& m)
                               {
                                   if (s < m.sets.Size())
                                   {
                                       m.sets[s].priority += 1;
                                   }
                               });
                       });
            MakeButton(*header, u8"-", 22.0f,
                       [self, s]()
                       {
                           self->Mutate(
                               [s](input::InputMap& m)
                               {
                                   if (s < m.sets.Size())
                                   {
                                       m.sets[s].priority -= 1;
                                   }
                               });
                       });
            MakeButton(*header, u8"+ Action", 70.0f,
                       [self, s]()
                       {
                           self->Mutate(
                               [s](input::InputMap& m)
                               {
                                   if (s >= m.sets.Size())
                                   {
                                       return;
                                   }
                                   input::Action action;
                                   action.name = String(u8"NewAction");
                                   m.sets[s].actions.PushBack(static_cast<input::Action&&>(action));
                               });
                       });
            MakeButton(*header, u8"x", 22.0f,
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

            for (usize a = 0; a < set.actions.Size(); ++a)
            {
                const input::Action& action = set.actions[a];
                auto row = MakeRow(18.0f);
                AddNameEditor(*row, action.name.AsView(),
                              [self, s, a](StringView value)
                              {
                                  String name(value);
                                  self->Mutate(
                                      [s, a, name](input::InputMap& m)
                                      {
                                          if (s < m.sets.Size() && a < m.sets[s].actions.Size())
                                          {
                                              m.sets[s].actions[a].name = name;
                                          }
                                      });
                              });
                MakeButton(*row, detail::KindName(action.kind), 60.0f,
                           [self, s, a]()
                           {
                               self->Mutate(
                                   [s, a](input::InputMap& m)
                                   {
                                       if (s >= m.sets.Size() || a >= m.sets[s].actions.Size())
                                       {
                                           return;
                                       }
                                       input::Action& act = m.sets[s].actions[a];
                                       act.kind = static_cast<input::ActionKind>(
                                           (static_cast<u8>(act.kind) + 1u) % 3u);
                                   });
                           });
                MakeButton(*row, detail::InteractionName(action.interaction.kind), 80.0f,
                           [self, s, a]()
                           {
                               self->Mutate(
                                   [s, a](input::InputMap& m)
                                   {
                                       if (s >= m.sets.Size() || a >= m.sets[s].actions.Size())
                                       {
                                           return;
                                       }
                                       input::Action& act = m.sets[s].actions[a];
                                       act.interaction.kind = static_cast<input::InteractionKind>(
                                           (static_cast<u8>(act.interaction.kind) + 1u) % 4u);
                                   });
                           });
                MakeButton(*row, u8"+ Binding", 74.0f,
                           [self, s, a]()
                           {
                               self->Mutate(
                                   [s, a](input::InputMap& m)
                                   {
                                       if (s >= m.sets.Size() || a >= m.sets[s].actions.Size())
                                       {
                                           return;
                                       }
                                       input::Action& act = m.sets[s].actions[a];
                                       input::Binding fresh;
                                       if (act.kind == input::ActionKind::Axis2D)
                                       {
                                           fresh.source = input::BindingSource::GamepadStick;
                                       }
                                       act.bindings.PushBack(fresh);
                                   });
                           });
                MakeButton(*row, u8"x", 22.0f,
                           [self, s, a]()
                           {
                               self->Mutate(
                                   [s, a](input::InputMap& m)
                                   {
                                       if (s < m.sets.Size() && a < m.sets[s].actions.Size())
                                       {
                                           m.sets[s].actions.RemoveAt(a);
                                       }
                                   });
                           });

                for (usize b = 0; b < action.bindings.Size(); ++b)
                {
                    const input::Binding& binding = action.bindings[b];
                    auto bindingRow = MakeRow(40.0f, 22.0f);
                    const bool isListening = m_listening && m_listenSet == s &&
                                             m_listenAction == a && m_listenBinding == b;
                    // Source cycles through the KIND's valid sources.
                    MakeButton(
                        *bindingRow, detail::SourceName(binding.source), 76.0f,
                        [self, s, a, b]()
                        {
                            self->Mutate(
                                [s, a, b](input::InputMap& m)
                                {
                                    if (s >= m.sets.Size() || a >= m.sets[s].actions.Size())
                                    {
                                        return;
                                    }
                                    auto& act = m.sets[s].actions[a];
                                    if (b >= act.bindings.Size())
                                    {
                                        return;
                                    }
                                    input::BindingSource valid[8];
                                    const usize n = detail::ValidSources(act.kind, valid);
                                    usize current = 0;
                                    for (usize i = 0; i < n; ++i)
                                    {
                                        if (valid[i] == act.bindings[b].source)
                                        {
                                            current = i;
                                            break;
                                        }
                                    }
                                    input::Binding fresh; // source change resets source-specifics
                                    fresh.source = valid[(current + 1) % n];
                                    act.bindings[b] = fresh;
                                });
                        });
                    AddLabel(*bindingRow,
                             isListening ? StringView(u8"<press an input...>")
                                         : detail::DescribeBinding(binding).AsView(),
                             1.0f);
                    MakeButton(*bindingRow, u8"Listen", 54.0f,
                               [self, s, a, b]() { self->BeginListen(s, a, b); });
                    MakeButton(*bindingRow, u8"x", 22.0f,
                               [self, s, a, b]()
                               {
                                   self->Mutate(
                                       [s, a, b](input::InputMap& m)
                                       {
                                           if (s >= m.sets.Size() || a >= m.sets[s].actions.Size())
                                           {
                                               return;
                                           }
                                           auto& bindings = m.sets[s].actions[a].bindings;
                                           if (b < bindings.Size())
                                           {
                                               bindings.RemoveAt(b);
                                           }
                                       });
                               });
                    BuildBindingDetail(s, a, b, binding);
                }

                // Processors (+ interaction window) on their own line.
                {
                    auto proc = MakeRow(40.0f, 20.0f);
                    if (action.interaction.kind != input::InteractionKind::None)
                    {
                        AddFloatField(*proc, u8"sec", action.interaction.seconds,
                                      [self, s, a](f32 v)
                                      {
                                          self->MutateAction(s, a, [v](input::Action& x)
                                                             { x.interaction.seconds = v; });
                                      });
                    }
                    if (action.kind != input::ActionKind::Button)
                    {
                        AddFloatField(*proc, u8"sens", action.processors.sensitivity,
                                      [self, s, a](f32 v)
                                      {
                                          self->MutateAction(s, a, [v](input::Action& x)
                                                             { x.processors.sensitivity = v; });
                                      });
                        AddFloatField(*proc, u8"grav", action.processors.gravity,
                                      [self, s, a](f32 v)
                                      {
                                          self->MutateAction(s, a, [v](input::Action& x)
                                                             { x.processors.gravity = v; });
                                      });
                        AddToggle(*proc, u8"snap", action.processors.snap,
                                  [self, s, a](bool v)
                                  {
                                      self->MutateAction(s, a, [v](input::Action& x)
                                                         { x.processors.snap = v; });
                                  });
                        AddFloatField(*proc, u8"curve", action.processors.responseExponent,
                                      [self, s, a](f32 v)
                                      {
                                          self->MutateAction(
                                              s, a, [v](input::Action& x)
                                              { x.processors.responseExponent = v; });
                                      });
                        AddToggle(*proc, u8"tScale", action.processors.timeScale,
                                  [self, s, a](bool v)
                                  {
                                      self->MutateAction(s, a, [v](input::Action& x)
                                                         { x.processors.timeScale = v; });
                                  });
                    }
                }
            }
        }

        auto footer = MakeRow(0.0f, 26.0f);
        MakeButton(*footer, u8"+ Add Set", 90.0f,
                   [self]()
                   {
                       self->Mutate(
                           [](input::InputMap& m)
                           {
                               input::ActionSet set;
                               set.name = String(u8"NewSet");
                               m.sets.PushBack(static_cast<input::ActionSet&&>(set));
                           });
                   });

        RefreshStatus();
        m_content->Invalidate();
    }

    void InputMapEditorPage::BeginListen(usize set, usize action, usize binding,
                                         i32 compositeDirection)
    {
        m_listening = true;
        m_listenSet = set;
        m_listenAction = action;
        m_listenBinding = binding;
        m_listenDirection = compositeDirection;
        if (compositeDirection >= 0)
        {
            // A composite direction rebind is always a single KEY.
            input::CaptureFilter keysOnly;
            keysOnly.mouseButtons = false;
            keysOnly.gamepadButtons = false;
            m_listenFilter = keysOnly;
            RequestRebuild();
            return;
        }
        // Filter by the action's declared kind: a Button rebind ignores stick noise,
        // an Axis2D rebind captures sticks only.
        input::CaptureFilter filter;
        if (set < m_map.sets.Size() && action < m_map.sets[set].actions.Size())
        {
            switch (m_map.sets[set].actions[action].kind)
            {
            case input::ActionKind::Button:
                break; // keys + mouse + pad buttons
            case input::ActionKind::Axis1D:
                filter.gamepadAxes = true;
                break;
            case input::ActionKind::Axis2D:
                filter.keys = false;
                filter.mouseButtons = false;
                filter.gamepadButtons = false;
                filter.gamepadSticks = true;
                break;
            }
        }
        m_listenFilter = filter;
        Rebuild();
    }

    void InputMapEditorPage::BuildBindingDetail(usize s, usize a, usize b,
                                                const input::Binding& binding)
    {
        using Source = input::BindingSource;
        const Source source = binding.source;
        const bool hasDeadZone = source == Source::MouseAxis || source == Source::GamepadAxis ||
                                 source == Source::GamepadStick || source == Source::TouchStick;
        const bool hasScale = source != Source::TouchButton;
        const bool hasInvert = source == Source::MouseAxis || source == Source::MouseDelta ||
                               source == Source::GamepadAxis || source == Source::GamepadStick ||
                               source == Source::TouchStick || source == Source::Composite2D;
        const bool hasDevice = source == Source::GamepadButton || source == Source::GamepadAxis ||
                               source == Source::GamepadStick;
        const bool hasRegion = source == Source::TouchButton || source == Source::TouchStick;
        InputMapEditorPage* self = this;

        auto detail = MakeRow(62.0f, 20.0f);
        if (hasDeadZone)
        {
            AddFloatField(
                *detail, u8"dz", binding.deadZone, [self, s, a, b](f32 v)
                { self->MutateBinding(s, a, b, [v](input::Binding& x) { x.deadZone = v; }); });
        }
        if (hasScale)
        {
            AddFloatField(
                *detail, u8"scale", binding.scale, [self, s, a, b](f32 v)
                { self->MutateBinding(s, a, b, [v](input::Binding& x) { x.scale = v; }); });
        }
        if (hasInvert)
        {
            AddToggle(*detail, u8"inv", binding.invert, [self, s, a, b](bool v)
                      { self->MutateBinding(s, a, b, [v](input::Binding& x) { x.invert = v; }); });
        }
        if (hasDevice)
        {
            AddFloatField(*detail, u8"pad", static_cast<f32>(binding.device),
                          [self, s, a, b](f32 v)
                          {
                              self->MutateBinding(s, a, b, [v](input::Binding& x)
                                                  { x.device = static_cast<i32>(v); });
                          });
        }
        if (source == Source::Composite2D)
        {
            AddToggle(
                *detail, u8"norm", binding.normalize, [self, s, a, b](bool v)
                { self->MutateBinding(s, a, b, [v](input::Binding& x) { x.normalize = v; }); });
            // Per-direction key capture: -X +X -Y +Y each Listen for one key.
            const StringView labels[] = {u8"-X", u8"+X", u8"-Y", u8"+Y"};
            for (u32 d = 0; d < 4; ++d)
            {
                String text(labels[d]);
                text += u8" ";
                const u32 code = d == 0   ? binding.negX
                                 : d == 1 ? binding.posX
                                 : d == 2 ? binding.negY
                                          : binding.posY;
                text += detail::KeyName(code);
                MakeButton(*detail, text.AsView(), 74.0f, [self, s, a, b, d]()
                           { self->BeginListen(s, a, b, static_cast<i32>(d)); });
            }
        }
        if (hasRegion)
        {
            AddFloatField(
                *detail, u8"rx", binding.regionX, [self, s, a, b](f32 v)
                { self->MutateBinding(s, a, b, [v](input::Binding& x) { x.regionX = v; }); });
            AddFloatField(
                *detail, u8"ry", binding.regionY, [self, s, a, b](f32 v)
                { self->MutateBinding(s, a, b, [v](input::Binding& x) { x.regionY = v; }); });
            AddFloatField(
                *detail, u8"rw", binding.regionW, [self, s, a, b](f32 v)
                { self->MutateBinding(s, a, b, [v](input::Binding& x) { x.regionW = v; }); });
            AddFloatField(
                *detail, u8"rh", binding.regionH, [self, s, a, b](f32 v)
                { self->MutateBinding(s, a, b, [v](input::Binding& x) { x.regionH = v; }); });
        }
        if (source == Source::TouchStick)
        {
            AddFloatField(
                *detail, u8"radius", binding.stickRadius, [self, s, a, b](f32 v)
                { self->MutateBinding(s, a, b, [v](input::Binding& x) { x.stickRadius = v; }); });
        }
    }

    void InputMapEditorPage::RefreshStatus()
    {
        if (m_listening)
        {
            m_status->SetText(u8"Listening... press the new input (Esc cancels)");
            return;
        }
        String error;
        if (!input::ValidateInputMap(m_map, &error))
        {
            String message(u8"Invalid: ");
            message += error;
            m_status->SetText(message.AsView());
        }
        else
        {
            m_status->SetText(
                u8"Sets > actions > bindings. Click names to rename; Listen rebinds.");
        }
    }
    const TypeInfo* InputMapPageFactory::PrimaryType() const
    {
        return &input::InputMapAsset::StaticType();
    }

    UniquePtr<EditorPage> InputMapPageFactory::CreatePage(EditorContext& context,
                                                          draconic::content::Instance& instance)
    {
        auto* page = DefaultAllocator().New<InputMapEditorPage>(context, *m_host, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }
}
