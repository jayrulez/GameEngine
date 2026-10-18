// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :action partition
//
// Action + ActionInterpolation: the animation/tween base. Ported from eepp's Scene::Action
// (include/eepp/scene/action.hpp) - abstract lifecycle (Start/Stop/Update/IsDone) driven by
// the ActionManager, targeting a Node, with a tag/id and an OnDone callback. eepp Time ->
// core::Duration. Object-derived so actions are RefPtr-owned by the manager and Cast<T>-able.
//
// The base only stores the target as a Node* (forward-declared) and never dereferences it;
// concrete actions that touch the node live in :actions (which imports the full Node).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:action;

import foundation.core; // Object, Function, Duration, Max, Min

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
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
        /// Whether it can change where its target is, how big, or how far its drawing reaches (an
        /// animation that could bring a node out of view into view). One that cannot asks for no
        /// frames while its target is out of view (E8b); it still runs, so it is caught up when the
        /// target shows. True unless an action knows better.
        [[nodiscard]] virtual bool CanMoveTarget() const { return true; }
        /// How much of the time its last Update was given ran past its end (E14): a group's next
        /// step starts with it, so a sequence keeps time whatever the frames.
        [[nodiscard]] virtual core::Duration GetOverrun() const { return core::Duration{}; }

        void SetTarget(Node* target)
        {
            m_target = target;
            OnTargetChange();
        }
        [[nodiscard]] Node* GetTarget() const noexcept { return m_target; }

        /// A weak hold on the target, resolved through its GUI's element table (Node::RunAction
        /// sets it): an action whose target is gone is dropped rather than run on freed memory.
        struct WeakTarget
        {
            const void* table = nullptr;
            u32 slot = 0;
            u32 generation = 0;
            Node* (*resolve)(const WeakTarget&) = nullptr;
        };
        void SetWeakTarget(const WeakTarget& weak) noexcept { m_weak = weak; }
        /// The target when it is alive (or when none was held weakly), null once it is gone.
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

    // Time-based action interpolating a [0,1] progress over a duration. Subclasses implement
    // OnStep(t) to apply the interpolated value to the target.
    class ActionInterpolation : public Action
    {
        RTTI_OBJECT(ActionInterpolation, Action)
    public:
        explicit ActionInterpolation(core::Duration duration) noexcept : m_duration(duration) {}

        void Start() override
        {
            m_elapsed = core::Duration{};
            m_started = true;
            m_done = false;
            OnStep(0.0f);
        }
        void Stop() override { m_done = true; }

        void Update(core::Duration elapsed) override
        {
            if (m_done)
                return;
            if (!m_started)
                Start();
            m_elapsed += elapsed;

            const f32 total = m_duration.AsSecondsF();
            f32 t = (total > 0.0f) ? (m_elapsed.AsSecondsF() / total) : 1.0f;
            t = core::Max(0.0f, core::Min(1.0f, t));
            m_progress = t;
            OnStep(t);

            if (t >= 1.0f)
            {
                m_done = true;
                FireDone();
            }
        }

        [[nodiscard]] bool IsDone() override { return m_done; }
        [[nodiscard]] f32 GetCurrentProgress() override { return m_progress; }
        [[nodiscard]] core::Duration GetTotalTime() override { return m_duration; }

    protected:
        // Apply progress t in [0,1] to the target.
        virtual void OnStep(f32 t) = 0;

        core::Duration m_duration;
        core::Duration m_elapsed;
        f32 m_progress = 0.0f;
        bool m_started = false;
        bool m_done = false;
    };

    RTTI_DEFINE_OBJECT(ActionInterpolation, "rtti::gui")
}
