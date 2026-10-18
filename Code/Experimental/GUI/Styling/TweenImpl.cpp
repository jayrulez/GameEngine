// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - a node's tweens (E14 of Specs/gui-lab-evolution-2.md): Node::Animate and StopAnimation,
// here because :node cannot import :tween (a tween targets a node), and the tweens' one count.

module;
#include "Core/Prelude.h"

module experimental.gui;

using namespace foundation::core;
namespace core = foundation::core;

namespace experimental::gui
{
    u64 NextTweenOwner() noexcept
    {
        static u64 next = 0;
        return ++next;
    }

    namespace
    {
        // The running tween of `property` on `node`, or null.
        Tween* RunningTween(Node& node, core::StringView property)
        {
            ActionManager* manager = node.GetActionManager();
            if (manager == nullptr)
                return nullptr;
            Tween* found = nullptr;
            manager->ForEachAction([&](Action& action)
            {
                if (Tween* tween = core::Cast<Tween>(&action); tween != nullptr && !tween->IsDone() &&
                                                               tween->ResolveTarget() == &node && tween->GetProperty() == property)
                    found = tween;
            });
            return found;
        }
    }

    RefPtr<Tween> Node::Animate(core::StringView property, core::StringView to, f32 seconds, core::StringView easing)
    {
        if (Tween* running = RunningTween(*this, property))
            running->Replace(); // the new one starts from the value it shows
        RefPtr<Tween> tween = core::MakeRef<Tween>(MemoryAllocator(), core::String(property), core::String(to), seconds,
                                                   ParseEasing(easing).ValueOr(Easing::Ease()));
        RunAction(tween);
        return tween;
    }

    bool Node::StopAnimation(core::StringView property)
    {
        bool stopped = false;
        if (Tween* running = RunningTween(*this, property))
        {
            running->Cancel();
            stopped = true;
        }
        if (UINode* node = core::Cast<UINode>(this))
            stopped = node->RemoveMotion(property) || stopped;
        return stopped;
    }
}
