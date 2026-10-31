// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - Node::Animate and StopAnimation, which live here because :node cannot import :tween (a
// tween targets a node). Also the one counter for tween owner marks, and GUIContext::AnimateData,
// which tweens a GUI data value.

module;
#include "Core/Prelude.h"

module foundation.gui;

using namespace foundation::core;
namespace core = foundation::core;

namespace foundation::gui
{
    u64 NextTweenOwner() noexcept
    {
        static u64 next = 0;
        return ++next;
    }

    namespace
    {
        // Calls `f(tween)` for each running tween, inside groups too.
        template <typename F>
        void VisitRunning(Action& action, F& f)
        {
            if (action.IsDone())
                return;
            if (Tween* tween = core::Cast<Tween>(&action))
                f(*tween);
            else if (TweenGroup* group = core::Cast<TweenGroup>(&action))
                group->ForEachRunning([&f](Action& step) { VisitRunning(step, f); });
        }
        template <typename F>
        void ForEachRunningTween(Node& node, F&& f)
        {
            if (ActionManager* manager = node.GetActionManager())
                manager->ForEachAction([&f](Action& action) { VisitRunning(action, f); });
        }
    }

    void ReplaceOthersOf(Tween& tween)
    {
        Node* target = tween.ResolveTarget();
        if (target == nullptr)
            return;
        ForEachRunningTween(*target, [&](Tween& other)
        {
            if (&other != &tween && other.ResolveTarget() == target && other.GetProperty() == tween.GetProperty())
                other.Replace(); // the new tween starts from the value the old one shows
        });
    }

    RefPtr<Tween> Node::CreateTween(core::StringView property, core::StringView to, f32 seconds, core::StringView easing)
    {
        RefPtr<Tween> tween = core::MakeRef<Tween>(MemoryAllocator(), core::String(property), core::String(to), seconds,
                                                   ParseEasing(easing).ValueOr(Easing::Ease()));
        TargetAction(*tween);
        return tween;
    }

    RefPtr<Tween> Node::Animate(core::StringView property, core::StringView to, f32 seconds, core::StringView easing)
    {
        RefPtr<Tween> tween = CreateTween(property, to, seconds, easing);
        RunAction(tween); // starting it replaces the tween already running
        return tween;
    }

    RefPtr<Tween> Node::SpringTo(core::StringView property, core::StringView to, f32 stiffness, f32 damping, f32 mass)
    {
        RefPtr<Tween> running;
        ForEachRunningTween(*this, [&](Tween& tween)
        {
            if (tween.IsSpring() && tween.ResolveTarget() == this && tween.GetProperty() == property)
                running = RefPtr<Tween>(&tween);
        });
        if (running)
        {
            running->Spring(stiffness, damping, mass);
            running->Retarget(to);
            return running;
        }
        RefPtr<Tween> tween = CreateTween(property, to, 0.0f);
        tween->Spring(stiffness, damping, mass);
        RunAction(tween);
        return tween;
    }

    RefPtr<Tween> GUIContext::CreateDataTween(core::StringView name, f64 to, f32 seconds, core::StringView easing)
    {
        RefPtr<Tween> tween = core::MakeRef<Tween>(MemoryAllocator(), core::String(name), FormatVariant(core::Variant::From<f64>(to)), seconds,
                                                   ParseEasing(easing).ValueOr(Easing::Ease()));
        tween->TargetData(m_data);
        return tween;
    }

    RefPtr<Tween> GUIContext::AnimateData(core::StringView name, f64 to, f32 seconds, core::StringView easing)
    {
        RefPtr<Tween> tween = CreateDataTween(name, to, seconds, easing);
        m_actionManager.AddAction(tween); // starting it replaces the tween moving the value
        return tween;
    }

    void Node::Play(RefPtr<TweenGroup> group)
    {
        if (group)
            RunAction(core::Move(group));
    }

    bool Node::StopAnimation(core::StringView property)
    {
        bool stopped = false;
        ForEachRunningTween(*this, [&](Tween& tween)
        {
            if (tween.ResolveTarget() == this && tween.GetProperty() == property)
            {
                tween.Cancel();
                stopped = true;
            }
        });
        if (UINode* node = core::Cast<UINode>(this))
            stopped = node->RemoveMotion(property) || stopped;
        return stopped;
    }
}
