// Draconic GUI - :action partition
//
// Action + ActionInterpolation: the animation/tween base. Ported from eepp's Scene::Action
// (include/eepp/scene/action.hpp) - abstract lifecycle (Start/Stop/Update/IsDone) driven by
// the ActionManager, targeting a Node, with a tag/id and an OnDone callback. eepp Time ->
// foundation::Duration. Object-derived so actions are RefPtr-owned by the manager and Cast<T>-able.
//
// The base only stores the target as a Node* (forward-declared) and never dereferences it;
// concrete actions that touch the node live in :actions (which imports the full Node).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:action;

import draconic.foundation; // Object, Function, Duration, Max, Min

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
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
        DRACONIC_OBJECT(Action, Object)
    public:
        using Callback = foundation::Function<void(Action&)>;

        virtual void Start() = 0;
        virtual void Stop() = 0;
        virtual void Update(foundation::Duration elapsed) = 0;
        [[nodiscard]] virtual bool IsDone() = 0;
        [[nodiscard]] virtual f32 GetCurrentProgress() = 0;
        [[nodiscard]] virtual foundation::Duration GetTotalTime() = 0;

        void SetTarget(Node* target)
        {
            m_target = target;
            OnTargetChange();
        }
        [[nodiscard]] Node* GetTarget() const noexcept { return m_target; }

        void SetId(u64 id) noexcept { m_id = id; }
        [[nodiscard]] u64 GetId() const noexcept { return m_id; }
        void SetTag(u64 tag) noexcept { m_tag = tag; }
        [[nodiscard]] u64 GetTag() const noexcept { return m_tag; }

        void SetOnDone(Callback callback) { m_onDone = foundation::Move(callback); }

    protected:
        virtual void OnTargetChange() {}
        void FireDone()
        {
            if (m_onDone)
                m_onDone(*this);
        }

        Node* m_target = nullptr; // non-owning
        Callback m_onDone;
        u64 m_id = 0;
        u64 m_tag = 0;
    };

    DRACONIC_DEFINE_OBJECT(Action, "draconic::gui")

    // Time-based action interpolating a [0,1] progress over a duration. Subclasses implement
    // OnStep(t) to apply the interpolated value to the target.
    class ActionInterpolation : public Action
    {
        DRACONIC_OBJECT(ActionInterpolation, Action)
    public:
        explicit ActionInterpolation(foundation::Duration duration) noexcept : m_duration(duration) {}

        void Start() override
        {
            m_elapsed = foundation::Duration{};
            m_started = true;
            m_done = false;
            OnStep(0.0f);
        }
        void Stop() override { m_done = true; }

        void Update(foundation::Duration elapsed) override
        {
            if (m_done)
                return;
            if (!m_started)
                Start();
            m_elapsed += elapsed;

            const f32 total = m_duration.AsSecondsF();
            f32 t = (total > 0.0f) ? (m_elapsed.AsSecondsF() / total) : 1.0f;
            t = foundation::Max(0.0f, foundation::Min(1.0f, t));
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
        [[nodiscard]] foundation::Duration GetTotalTime() override { return m_duration; }

    protected:
        // Apply progress t in [0,1] to the target.
        virtual void OnStep(f32 t) = 0;

        foundation::Duration m_duration;
        foundation::Duration m_elapsed;
        f32 m_progress = 0.0f;
        bool m_started = false;
        bool m_done = false;
    };

    DRACONIC_DEFINE_OBJECT(ActionInterpolation, "draconic::gui")
}
