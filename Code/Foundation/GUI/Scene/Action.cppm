// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :action partition
//
// Action: the base class of what runs on a GUI's clock (the tweens, a widget's TickAction). Adapted from
// eepp's Scene::Action (include/eepp/scene/action.hpp): an abstract lifecycle
// (Start/Stop/Update/IsDone) driven by the ActionManager, targeting a Node, with a tag, an id and
// an OnDone callback. Object-derived, so actions are RefPtr-owned by the manager and Cast<T>-able.
// The base stores the target as a Node* (forward-declared) and never dereferences it; actions that
// touch the node live where the full Node is imported.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:action;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class Node;

    enum class ActionType
    {
        OnStart,
        OnStop,
        OnUpdate,
        OnDone
    };

    class Action : public Object
    {
        RTTI_OBJECT(Action, Object)
    public:
        using Callback = core::Function<void(Action&)>;

        virtual void Start() = 0;
        virtual void Stop() = 0;
        virtual void Update(core::Duration elapsed) = 0;
        [[nodiscard]] virtual bool IsDone() = 0;
        [[nodiscard]] virtual f32 GetCurrentProgress() = 0;
        [[nodiscard]] virtual core::Duration GetTotalTime() = 0;
        /// Whether the action can change its target's position, size or drawn extent (an animation
        /// that could bring an out-of-view node into view). An action that cannot asks for no
        /// frames while its target is out of view; it still runs, so it is up to date when the
        /// target shows. Returns true unless an action overrides it.
        [[nodiscard]] virtual bool CanMoveTarget() const { return true; }
        /// How much of the time given to the last Update ran past the action's end. A group starts
        /// its next step with this overrun, so a sequence keeps time whatever the frame lengths.
        [[nodiscard]] virtual core::Duration GetOverrun() const { return core::Duration{}; }

        void SetTarget(Node* target)
        {
            m_target = target;
            OnTargetChange();
        }
        [[nodiscard]] Node* GetTarget() const noexcept { return m_target; }

        /// A weak reference to the target, resolved through its GUI's element table and set by
        /// Node::RunAction. An action whose target is gone is dropped rather than run on freed
        /// memory.
        struct WeakTarget
        {
            const void* table = nullptr;
            u32 slot = 0;
            u32 generation = 0;
            Node* (*resolve)(const WeakTarget&) = nullptr;
        };
        void SetWeakTarget(const WeakTarget& weak) noexcept { m_weak = weak; }
        /// Returns the target while it is alive (or when it is not held weakly), and null once it
        /// is gone.
        [[nodiscard]] Node* ResolveTarget() const { return m_weak.resolve != nullptr ? m_weak.resolve(m_weak) : m_target; }

        void SetId(u64 id) noexcept { m_id = id; }
        [[nodiscard]] u64 GetId() const noexcept { return m_id; }
        void SetTag(u64 tag) noexcept { m_tag = tag; }
        [[nodiscard]] u64 GetTag() const noexcept { return m_tag; }

        void SetOnDone(Callback callback) { m_onDone = core::Move(callback); }

    protected:
        virtual void OnTargetChange() {}
        void FireDone()
        {
            if (m_onDone)
                m_onDone(*this);
        }

        Node* m_target = nullptr; // non-owning; read through ResolveTarget when held weakly
        WeakTarget m_weak{};
        Callback m_onDone;
        u64 m_id = 0;
        u64 m_tag = 0;
    };

    RTTI_DEFINE_OBJECT(Action, "rtti::gui")
}
