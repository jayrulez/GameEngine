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

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class SceneRoot : public Node
    {
        RTTI_OBJECT(SceneRoot, Node)
    public:
        // The element table is this GUI's own (MemoryAllocator is valid in a constructor).
        SceneRoot() : m_elements(core::MakeRef<ElementTable>(MemoryAllocator())) {}
        ~SceneRoot() override { m_elements->Close(); }

        // Advance the tree by `elapsed`: tick actions, run the per-frame hook, then drain
        // deferred tree edits (e.g. queued Node::Close removals).
        void Update(core::Duration elapsed)
        {
            m_actionManager.Update(elapsed);
            OnUpdate(elapsed);
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
            return NeedsRedraw() || !m_actionManager.IsEmpty() || IsLayoutDirty() || HasLayoutDirtyDescendant();
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
        MutationQueue m_mutationQueue;
        EventDispatcher m_eventDispatcher{this}; // root = this SceneRoot
        RefPtr<ElementTable> m_elements;          // what weak references to this GUI's nodes resolve through
        ElementSet m_elementSet = ElementSet::All();
        f32 m_displayScale = 1.0f;
        u32 m_lastLayoutCount = 0;
        core::Float2 m_viewport{0.0f, 0.0f};

        void ApplyViewport()
        {
            if (m_viewport.x > 0.0f || m_viewport.y > 0.0f)
                SetSize(core::Float2{m_viewport.x / m_displayScale, m_viewport.y / m_displayScale});
        }
    };
}
