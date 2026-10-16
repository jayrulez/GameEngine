// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :scene_root partition
//
// SceneRoot: the root of one GUI's widget tree AND its coordinator (eepp's Scene::SceneNode, by
// role). A Node subclass that sits at the top; it owns the ActionManager, the MutationQueue, the
// event dispatcher, the element table and set, and its display scale. Update ticks running
// actions then drains deferred tree edits. Drawing is inherited from Node::Draw.
//
// Each root is scaled on its own (one per window, or per render target: a panel in the world, a
// texture): its tree lays out in logical units, which its display scale (the window's content
// scale times a UI scale) maps to device pixels, through the root's own transform, so drawing and
// hit testing agree and two windows on monitors of different density each keep their own.
//
// Nodes reach the coordinator through Node's virtual GetActionManager()/GetMutationQueue(),
// which walk up the parent chain; SceneRoot overrides them to return its own.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:scene_root;

import foundation.core; // Duration
import :node;
import :element_types;
import :action_manager;
import :mutation_queue;
import :event_dispatcher;
import :media_query; // MediaContext
import :reactive;    // EffectScheduler
import :data_model;
import :commands;
import :keymap;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class SceneRoot : public Node
    {
        RTTI_OBJECT(SceneRoot, Node)
    public:
        // The element table is this GUI's own (MemoryAllocator is valid in a constructor).
        SceneRoot()
            : m_data(core::MakeRef<DataModel>(MemoryAllocator())), m_elements(core::MakeRef<ElementTable>(MemoryAllocator()))
        {
            m_eventDispatcher.SetKeystrokeRouter([this](const Keystroke& stroke) { return RouteKeystroke(stroke); });
        }
        ~SceneRoot() override { m_elements->Close(); }

        // Advance the tree by `elapsed`: tick actions, run the per-frame hook, run the bindings
        // whose values changed, then drain deferred tree edits (e.g. queued Node::Close removals).
        void Update(core::Duration elapsed)
        {
            m_actionManager.Update(elapsed);
            m_eventDispatcher.Tick(static_cast<f32>(elapsed.AsSecondsF()));
            if (!m_chord.IsEmpty() && (m_chordAge += static_cast<f32>(elapsed.AsSecondsF())) > kChordTimeout)
                m_chord.Clear(); // a chord not finished in time is dropped
            OnUpdate(elapsed);
            m_lastBindingCount = m_effects.Flush();
            m_mutationQueue.Drain();
            UpdateLayout();
        }

        /// Runs the layout pass: every container that requested a layout, parents first, again
        /// while a layout requests more (a child's new size can move its parent), at most a few
        /// rounds. A host runs it after styling and before drawing; Update runs it too.
        void UpdateLayout()
        {
            m_lastLayoutCount = 0;
            for (i32 round = 0; round < 8 && (IsLayoutDirty() || HasLayoutDirtyDescendant()); ++round)
                m_lastLayoutCount += RunLayout();
        }
        /// How many containers the last layout pass laid out (none in a frame where nothing moved).
        [[nodiscard]] u32 LastLayoutCount() const noexcept { return m_lastLayoutCount; }

        /// Whether this GUI has anything to show that it has not drawn: something invalidated,
        /// an animation or another action running, or a layout waiting. A host skips a frame that
        /// needs none: an idle GUI costs nothing.
        [[nodiscard]] bool NeedsFrame() const noexcept
        {
            return NeedsRedraw() || !m_actionManager.IsEmpty() || IsLayoutDirty() || HasLayoutDirtyDescendant() ||
                   m_effects.HasPending() || m_eventDispatcher.IsHoldingDirection();
        }

        /// How many bindings the last Update ran (none in a frame where no bound value changed).
        [[nodiscard]] u32 LastBindingCount() const noexcept { return m_lastBindingCount; }
        /// This GUI's named values, which markup and scripts bind to.
        [[nodiscard]] DataModel& GetData() noexcept { return *m_data; }
        [[nodiscard]] const RefPtr<DataModel>& GetDataRef() const noexcept { return m_data; }

        [[nodiscard]] EffectScheduler* GetEffectScheduler() override { return &m_effects; }

        // === Commands (G7) ===
        [[nodiscard]] CommandSet& GetCommands() noexcept { return m_commands; }
        /// The command `id`, defined now if new.
        Command& DefineCommand(core::StringView id) { return m_commands.Define(id, MemoryAllocator()); }

        /// Runs command `id` along the focus path (the root alone when nothing here is focused):
        /// capture handlers from the root down, then bubble handlers from the focused widget up, the
        /// first bubble handler stopping it unless it sets Propagate. False when it is disabled or
        /// nothing handled it.
        bool RunCommand(core::StringView id)
        {
            if (const Command* command = m_commands.Find(id); command != nullptr && !Untracked([&] { return command->IsEnabled(); }))
                return false;
            Array<Node*> path; // focused first, this root last
            for (Node* n = FocusInThisTree(); n != nullptr; n = n->GetParent())
                path.PushBack(n);
            bool ran = false;
            CommandEvent event{id, CommandPhase::Capture, false};
            for (usize i = path.Size(); i-- > 0;)
                ran |= path[i]->RunCommandHandlers(event);
            event.Phase = CommandPhase::Bubble;
            for (Node* n : path)
            {
                event.Propagate = false;
                if (n->RunCommandHandlers(event))
                {
                    ran = true;
                    if (!event.Propagate)
                        break;
                }
            }
            return ran;
        }

        /// This GUI's bindings from keys and gamepad buttons to commands.
        [[nodiscard]] Keymap& GetKeymap() noexcept { return m_keymap; }

        /// A keystroke no widget used: matched against the keymap in the focus path's contexts (a
        /// chord waits for its next keystroke); a match runs its command. True when it was taken
        /// (a match, an unbinding, or the start of a chord).
        bool RouteKeystroke(const Keystroke& stroke)
        {
            Array<KeyContextLayer> stack; // root first, one layer per node declaring a context
            {
                Array<Node*> path;
                for (Node* n = FocusInThisTree(); n != nullptr; n = n->GetParent())
                    path.PushBack(n);
                KeyContextLayer layer;
                for (usize i = path.Size(); i-- > 0;)
                    if (path[i]->GetKeyContext().Size() != 0)
                    {
                        layer.Declare(path[i]->GetKeyContext());
                        stack.PushBack(layer);
                    }
            }
            m_chord.PushBack(stroke);
            m_chordAge = 0.0f;
            core::String command;
            KeymapMatch match = m_keymap.Match(m_chord.AsSpan(), stack.AsSpan(), command);
            if (match == KeymapMatch::None && m_chord.Size() > 1)
            {
                m_chord.Clear(); // a chord broken off: the keystroke alone
                m_chord.PushBack(stroke);
                match = m_keymap.Match(m_chord.AsSpan(), stack.AsSpan(), command);
            }
            if (match == KeymapMatch::Pending)
                return true;
            m_chord.Clear();
            if (match == KeymapMatch::None)
                return false;
            if (command.Size() == 0)
                return false; // unbound here: as if no binding
            (void)RunCommand(command.AsView());
            return true;
        }

        /// Whether command `id` is enabled and a handler for it is on the focus path: a read a
        /// binding follows (focus, the command's state, the handlers), so a control bound to the
        /// command greys and lights as focus moves.
        [[nodiscard]] bool IsCommandAvailable(core::StringView id)
        {
            m_commands.TrackChanges();
            (void)m_eventDispatcher.ObserveFocus();
            if (const Command* command = m_commands.Find(id); command != nullptr && !command->IsEnabled())
                return false;
            for (Node* n = FocusInThisTree(); n != nullptr; n = n->GetParent())
                if (n->HandlesCommand(id))
                    return true;
            return false;
        }
        [[nodiscard]] ActionManager* GetActionManager() override { return &m_actionManager; }
        [[nodiscard]] MutationQueue* GetMutationQueue() override { return &m_mutationQueue; }
        [[nodiscard]] EventDispatcher* GetEventDispatcher() override { return &m_eventDispatcher; }
        [[nodiscard]] ElementTable* GetElementTable() override { return m_elements.Get(); }
        [[nodiscard]] const ElementSet* GetElementSet() const override { return &m_elementSet; }

        /// Device pixels per logical unit: the window's content scale times a UI scale. The tree
        /// lays out, styles and handles input in logical units; the root's transform maps them to
        /// device pixels (input in device pixels hit-tests through it). The size follows: the
        /// viewport's device pixels over the scale.
        void SetDisplayScale(f32 scale)
        {
            m_displayScale = core::Max(scale, 0.01f);
            Transformable::SetScale(core::Float2{m_displayScale, m_displayScale});
            Invalidate();
            ApplyViewport();
        }
        [[nodiscard]] f32 GetDisplayScale() const noexcept { return m_displayScale; }

        /// The device pixels this root covers (its window's or target's); its size is them in
        /// logical units.
        void SetViewportSize(core::Float2 devicePixels)
        {
            m_viewport = devicePixels;
            ApplyViewport();
        }
        [[nodiscard]] core::Float2 GetViewportSize() const noexcept { return m_viewport; }

        /// What this root's media queries see: its logical size, and its density (96 dpi at 1x).
        [[nodiscard]] MediaContext GetMediaContext() const noexcept
        {
            return MediaContext{GetSize().x, GetSize().y, 96.0f * m_displayScale};
        }
        /// What this GUI's markup and scripts may create (every registered element by default).
        void SetElementSet(ElementSet set) { m_elementSet = core::Move(set); }

        // Per-frame hook for subclasses (layout, timers, ...).
        virtual void OnUpdate(core::Duration elapsed) { (void)elapsed; }

    private:
        ActionManager m_actionManager;
        EffectScheduler m_effects;  // the bindings waiting for this frame
        CommandSet m_commands;      // this GUI's commands
        Keymap m_keymap;            // keys and gamepad buttons to commands
        Array<Keystroke> m_chord;   // a chord typed so far
        f32 m_chordAge = 0.0f;
        static constexpr f32 kChordTimeout = 1.0f;
        RefPtr<DataModel> m_data;   // shared with the bindings to it
        u32 m_lastBindingCount = 0;
        MutationQueue m_mutationQueue;
        EventDispatcher m_eventDispatcher{this}; // root = this SceneRoot
        RefPtr<ElementTable> m_elements;          // what weak references to this GUI's nodes resolve through
        ElementSet m_elementSet = ElementSet::All();
        f32 m_displayScale = 1.0f;
        u32 m_lastLayoutCount = 0;
        core::Float2 m_viewport{0.0f, 0.0f};

        // The focused node when it is in this tree, else this root.
        [[nodiscard]] Node* FocusInThisTree()
        {
            Node* focus = m_eventDispatcher.GetFocusNode();
            for (Node* n = focus; n != nullptr; n = n->GetParent())
                if (n == this)
                    return focus;
            return this;
        }

        void ApplyViewport()
        {
            if (m_viewport.x > 0.0f || m_viewport.y > 0.0f)
                SetSize(core::Float2{m_viewport.x / m_displayScale, m_viewport.y / m_displayScale});
        }
    };
}
