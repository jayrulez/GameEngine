// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Input - the `editor.input` module.
//
// InputMapPage: the editing surface for InputMapAsset, its sets of actions and each action's
// bindings, laid out two ways to compare: an outline (each set a collapsible section of its
// actions) or two panes (the sets and actions listed, the selected one edited). Both are made of
// the same editors: a set's name and priority; an action's name, kind and interaction; its
// bindings as cards, each input a key cap to click and then press the new input (CaptureBinding
// polled per frame, filtered by the action's kind), its settings named in full; Add Binding as a
// menu of the sources the kind takes; and an axis's processing. Every mutation is one UNDOABLE
// command over whole-map snapshots (the map is small data); a field's edits merge into one. Save
// validates first: a kind-mismatched map never reaches the cook.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"
#include <cstdlib>

export module editor.input;

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

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace ui = foundation::ui;
    namespace input = foundation::input;

    namespace detail
    {
        // Binding + enum labels now live in foundation.input (:binding_names); re-exported here so
        // this page and its impl keep calling them as detail::<name>.
        using input::DescribeBinding;
        using input::InteractionName;
        using input::KeyName;
        using input::KindName;
        using input::PadButtonName;
        using input::SourceName;
        using input::ValidSources;
    }

    /// The input map editor's headless edits: what the rows' clicks and the listen capture do
    /// to the map, without a page. Every lookup is guarded (an out-of-range set, action or
    /// binding is a no-op or null), since a row can outlive the map it was built from.
    namespace input_map_edit
    {
        [[nodiscard]] inline input::ActionSet* SetAt(input::InputMap& map, usize set)
        {
            return set < map.sets.Size() ? &map.sets[set] : nullptr;
        }
        [[nodiscard]] inline input::Action* ActionAt(input::InputMap& map, usize set, usize action)
        {
            input::ActionSet* s = SetAt(map, set);
            return (s != nullptr && action < s->actions.Size()) ? &s->actions[action] : nullptr;
        }
        [[nodiscard]] inline bool HasBinding(input::InputMap& map, usize set, usize action,
                                             usize binding)
        {
            const input::Action* a = ActionAt(map, set, action);
            return a != nullptr && binding < a->bindings.Size();
        }
        /// A new binding for an action of `kind`: a stick for a 2D axis, else a key.
        [[nodiscard]] inline input::Binding FreshBinding(input::ActionKind kind)
        {
            input::Binding fresh;
            if (kind == input::ActionKind::Axis2D)
            {
                fresh.source = input::BindingSource::GamepadStick;
            }
            return fresh;
        }
        /// The next valid source for the kind after `current`'s (wrapping), on a FRESH binding:
        /// a source change resets the source-specific fields.
        [[nodiscard]] inline input::Binding CycleSource(input::ActionKind kind,
                                                        const input::Binding& current)
        {
            input::BindingSource valid[8];
            const usize n = input::ValidSources(kind, valid);
            usize index = 0;
            for (usize i = 0; i < n; ++i)
            {
                if (valid[i] == current.source)
                {
                    index = i;
                    break;
                }
            }
            input::Binding fresh;
            fresh.source = valid[(index + 1) % n];
            return fresh;
        }
        /// A fresh binding of `source`, for the Add Binding menu.
        [[nodiscard]] inline input::Binding FreshBinding(input::BindingSource source)
        {
            input::Binding fresh;
            fresh.source = source;
            return fresh;
        }
        /// A source as the page names it: what the user plugs in or touches.
        [[nodiscard]] inline StringView SourceTitle(input::BindingSource source)
        {
            switch (source)
            {
            case input::BindingSource::Key:
                return u8"Key";
            case input::BindingSource::MouseButton:
                return u8"Mouse button";
            case input::BindingSource::MouseAxis:
                return u8"Mouse axis";
            case input::BindingSource::MouseDelta:
                return u8"Mouse movement";
            case input::BindingSource::GamepadButton:
                return u8"Gamepad button";
            case input::BindingSource::GamepadAxis:
                return u8"Gamepad axis";
            case input::BindingSource::GamepadStick:
                return u8"Gamepad stick";
            case input::BindingSource::Composite2D:
                return u8"Four keys";
            case input::BindingSource::TouchButton:
                return u8"Touch button";
            case input::BindingSource::TouchStick:
                return u8"Touch stick";
            }
            return u8"?";
        }
        /// An action kind as the page names it.
        [[nodiscard]] inline StringView KindTitle(input::ActionKind kind)
        {
            switch (kind)
            {
            case input::ActionKind::Button:
                return u8"Button";
            case input::ActionKind::Axis1D:
                return u8"1D axis";
            case input::ActionKind::Axis2D:
                return u8"2D axis";
            }
            return u8"?";
        }

        /// What a listen for an action of `kind` captures: a Button rebind ignores stick
        /// noise, an Axis2D rebind captures sticks only; one Composite2D DIRECTION is always a
        /// single key.
        [[nodiscard]] inline input::CaptureFilter FilterFor(input::ActionKind kind,
                                                            bool compositeDirection)
        {
            input::CaptureFilter filter;
            if (compositeDirection)
            {
                filter.mouseButtons = false;
                filter.gamepadButtons = false;
                return filter;
            }
            switch (kind)
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
            return filter;
        }
        /// Lands a capture: the whole binding, or (direction 0..3 = -X +X -Y +Y) just the
        /// captured KEY code into that Composite2D slot. Out-of-range targets are ignored.
        inline void ApplyCapture(input::InputMap& map, usize set, usize action, usize binding,
                                 i32 direction, const input::Binding& captured)
        {
            input::Action* a = ActionAt(map, set, action);
            if (a == nullptr || binding >= a->bindings.Size())
            {
                return;
            }
            input::Binding& target = a->bindings[binding];
            if (direction < 0)
            {
                target = captured;
                return;
            }
            u32* slot = direction == 0   ? &target.negX
                        : direction == 1 ? &target.posX
                        : direction == 2 ? &target.negY
                                         : &target.posY;
            *slot = captured.code;
        }
    }

    class InputMapEditorPage final : public app::UIEditorPage
    {
    public:
        /// How the map is laid out, the two ways to compare (user 2026-10-10): one outline, each
        /// set a collapsible section of its actions; or two panes, the sets and actions listed on
        /// the left and the selected one edited on the right. Both build from the same editors.
        enum class Layout : u8
        {
            Outline,
            TwoPanes,
        };

        InputMapEditorPage(EditorContext& context, runtime::IApplicationHost&,
                           foundation::content::Instance& instance);

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] ui::View* ContentView() override { return m_content.Get(); }

        [[nodiscard]] Status Save() override;

        // Per-frame: the toolbar's state, and rebind capture while listening.
        void OnUpdate(runtime::IApplicationHost& host, f32) override;

        // For tests: the map as edited, the layout, the two-pane selection.
        [[nodiscard]] const input::InputMap& Map() const noexcept { return m_map; }
        [[nodiscard]] Layout CurrentLayout() const noexcept { return m_layout; }
        void SetLayout(Layout layout);
        /// Select a set (`action` < 0) or one of its actions, in the two-pane layout.
        void Select(usize set, isize action);
        [[nodiscard]] usize SelectedSet() const noexcept { return m_selectedSet; }
        [[nodiscard]] isize SelectedAction() const noexcept { return m_selectedAction; }
        /// Whether the outline shows a set's actions.
        [[nodiscard]] bool IsSetCollapsed(usize set) const;
        void SetSetCollapsed(usize set, bool collapsed);
        /// Add a binding of `source` to an action (the Add Binding menu's choice).
        void AddBinding(usize set, usize action, input::BindingSource source);
        /// Start (or, on the same target, cancel) listening for a binding's new input.
        void BeginListen(usize set, usize action, usize binding, i32 compositeDirection = -1);
        [[nodiscard]] bool IsListening() const noexcept { return m_listening; }
        /// The page's body, rebuilt now (tests; the page itself defers through RequestRebuild).
        void RebuildNow() { Rebuild(); }
        [[nodiscard]] ui::FlexLayout& Body() const noexcept { return *m_body; }

    private:
        // Every mutation = one undoable command over whole-map snapshots (small data). A field
        // edit is quiet (the field stays, no rebuild under it) and merges with the edit before
        // it on the same field, so dragging a value is one step; undo and redo rebuild.
        class MapEditCommand final : public IEditorCommand
        {
        public:
            MapEditCommand(InputMapEditorPage& page, input::InputMap before, input::InputMap after,
                           String mergeKey)
                : m_page(&page), m_before(static_cast<input::InputMap&&>(before)),
                  m_after(static_cast<input::InputMap&&>(after)), m_mergeKey(static_cast<String&&>(mergeKey))
            {
            }
            [[nodiscard]] bool Execute() override
            {
                m_page->m_map = m_after;
                if (!m_page->m_quietEdit)
                {
                    m_page->RequestRebuild();
                }
                return true;
            }
            void Undo() override
            {
                m_page->m_map = m_before;
                m_page->RequestRebuild();
            }
            [[nodiscard]] StringView TypeId() const override { return u8"input-map-edit"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto* earlier = static_cast<MapEditCommand*>(&previous);
                if (m_mergeKey.IsEmpty() || earlier->m_mergeKey != m_mergeKey)
                {
                    return false;
                }
                earlier->m_after = m_after;
                return true;
            }

        private:
            InputMapEditorPage* m_page;
            input::InputMap m_before;
            input::InputMap m_after;
            String m_mergeKey;
        };

        template <typename Fn>
        void Mutate(Fn&& fn)
        {
            MutateWith(String(), false, static_cast<Fn&&>(fn));
        }
        /// A field's edit: no rebuild, merged with this field's edit before it.
        template <typename Fn>
        void MutateField(StringView field, Fn&& fn)
        {
            MutateWith(String(field), true, static_cast<Fn&&>(fn));
        }
        template <typename Fn>
        void MutateWith(String mergeKey, bool quiet, Fn&& fn)
        {
            input::InputMap before = m_map;
            input::InputMap after = m_map;
            fn(after);
            m_quietEdit = quiet;
            (void)Commands().Execute(UniquePtr<IEditorCommand>(
                Allocator().New<MapEditCommand>(*this, static_cast<input::InputMap&&>(before),
                                                static_cast<input::InputMap&&>(after),
                                                static_cast<String&&>(mergeKey)),
                Allocator()));
            m_quietEdit = false;
        }

        template <typename Apply>
        void MutateAction(usize s, usize a, Apply&& apply)
        {
            Mutate(
                [s, a, apply](input::InputMap& m)
                {
                    if (input::Action* action = input_map_edit::ActionAt(m, s, a))
                    {
                        apply(*action);
                    }
                });
        }

        /// Defer a Rebuild through the UI MutationQueue - call this instead of Rebuild() from anything
        /// that runs DURING UI event dispatch (a row/button click, an undo/redo, a rebind). Rebuilding
        /// the rows in-line frees the clicked button, then FireClick + DispatchMouseUp dereference the
        /// freed view -> crash; the queue runs the rebuild AFTER dispatch drains. Rebuild() itself stays
        /// for the initial (setup-time) build.
        void RequestRebuild();
        void Rebuild();
        void BuildOutline();
        void BuildTwoPanes();

        // The editors both layouts are made of.
        void BuildSetFields(ui::FlexLayout& column, usize s);
        void BuildActionHeader(ui::FlexLayout& column, usize s, usize a, bool withName);
        void BuildBindings(ui::FlexLayout& column, usize s, usize a);
        void BuildBindingCard(ui::FlexLayout& column, usize s, usize a, usize b);
        void BuildBindingSettings(ui::FlexLayout& column, usize s, usize a, usize b);
        void BuildProcessing(ui::FlexLayout& column, usize s, usize a);
        void ShowAddBindingMenu(ui::View& anchor, usize s, usize a);

        // Small pieces.
        [[nodiscard]] RefPtr<ui::FlexLayout> Row(f32 spacing = 8.0f);
        [[nodiscard]] RefPtr<ui::FlexLayout> Column(f32 spacing = 6.0f);
        ui::Label* AddLabel(ui::FlexLayout& row, StringView text, f32 size = 12.0f, bool dim = false,
                            f32 width = 0.0f);
        void AddHeading(ui::FlexLayout& column, StringView text);
        void AddNote(ui::FlexLayout& column, StringView text);
        /// A rounded panel around `content` (a binding, an action in the outline).
        RefPtr<ui::Panel> Card(ui::View& content, f32 fill = 0.035f);
        ui::EditText* AddNameField(ui::FlexLayout& row, StringView value, Function<void(StringView)> commit,
                                   f32 width = 0.0f);
        ui::NumericField* AddNumber(ui::FlexLayout& row, StringView label, StringView tooltip, f64 value,
                                    f64 least, f64 most, u32 decimals, StringView field,
                                    Function<void(input::InputMap&, f64)> apply);
        ui::CheckBox* AddCheck(ui::FlexLayout& row, StringView label, StringView tooltip, bool value,
                               Function<void(input::InputMap&, bool)> apply);
        ui::ComboBox* AddChoice(ui::FlexLayout& row, Span<const StringView> items, i32 selected, f32 width,
                                Function<void(input::InputMap&, i32)> apply);
        /// A key cap showing an input: click it, then press the new input. `direction` >= 0 is
        /// one key of a four-key composite.
        ui::Button* AddKeycap(ui::FlexLayout& row, StringView text, usize s, usize a, usize b,
                              i32 direction, f32 minWidth = 0.0f);
        ui::IconButton* AddIcon(ui::FlexLayout& row, ui::SVGDrawable* icon, StringView tooltip,
                                Function<void()> onClick, f32 size = 14.0f);

        void RefreshStatus();

        EditorContext* m_context = nullptr;
        String m_title;
        input::InputMap m_map;

        RefPtr<ui::View> m_content;
        RefPtr<app::PageToolbar> m_toolbar;
        RefPtr<ui::FlexLayout> m_body;     // rebuilt: the layout in use
        RefPtr<ui::Label> m_status;        // validation, or what listening waits for
        RefPtr<ui::ComboBox> m_layoutChoice;

        Layout m_layout = Layout::Outline;
        usize m_selectedSet = 0;
        isize m_selectedAction = 0; // < 0: the set itself
        Array<String> m_collapsed;  // the outline's collapsed sets, by name
        bool m_quietEdit = false;   // a field's edit is executing: no rebuild under it
        // The scrolls live across rebuilds (only their content is rebuilt), so an edit never
        // throws the view back to the top.
        RefPtr<ui::ScrollView> m_outlineScroll;
        RefPtr<ui::ScrollView> m_listScroll;
        RefPtr<ui::ScrollView> m_detailScroll;

        bool m_listening = false;
        usize m_listenSet = 0;
        usize m_listenAction = 0;
        usize m_listenBinding = 0;
        i32 m_listenDirection = -1; // >= 0: capturing one Composite2D direction key
        input::CaptureFilter m_listenFilter;
    };

    class InputMapPageFactory final : public IEditorPageFactory
    {
    public:
        explicit InputMapPageFactory(runtime::IApplicationHost& host) : m_host(&host) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
    };

    /// The editor executable's entry point for the input plugin.
    inline void RegisterInputEditor(EditorContext& context, runtime::IApplicationHost& host)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<InputMapPageFactory>(host), editor::EditorRootAllocator()));
    }

}
