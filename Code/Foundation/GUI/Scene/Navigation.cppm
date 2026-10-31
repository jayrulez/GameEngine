// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :navigation partition
//
// Directional focus navigation, automatic by default: a geometric picker chooses the best
// focusable widget in the pressed direction, so most screens need no authoring. A candidate must
// lie beyond the focused widget in that direction. Candidates that overlap it on the cross axis
// come first (a neighbour in the same row beats a widget that is diagonally nearer), then the
// nearest along the axis, then the nearest across it. Hidden, disabled and clipped-away widgets
// are skipped.
//
// Where the geometry gives the wrong answer, a widget can name where a direction goes (nav-up,
// nav-down, nav-left, nav-right: an id, or none to stop there). Such an override is followed
// first.
//
// Focus scopes (Node::SetNavScope) bound the search. Navigation stays inside the focused widget's
// scope, and at its edge it passes to the enclosing scope, wraps to the scope's other side, or
// stays put (contain, for a dialog). Entering a scope from outside lands on the widget last
// focused in it, else on its default focus, else on the picked widget.

module;
#include "Core/Prelude.h"

export module foundation.gui:navigation;

import foundation.core; // Array, Max, Min, Abs
import :rect;
import :node;
import :ui_widget; // ids (for a scope's default focus)

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    enum class NavDirection : u8
    {
        Up,
        Down,
        Left,
        Right,
    };

    /// The outcome for one candidate of a directional move.
    enum class NavVerdict : u8
    {
        Chosen,  ///< the best: focus went there (or into the scope containing it)
        Beaten,  ///< beyond the focused widget in that direction, but with a worse score
        Behind,  ///< not beyond the focused widget in that direction
        Clipped, ///< hidden by a clipping ancestor
    };
    /// One candidate a directional move weighed: its bounds (in the GUI's world space), its name,
    /// its score (lower is better; 1e6 and above means it does not overlap the focused widget
    /// across the move) and its outcome.
    struct NavCandidate
    {
        Rect Bounds;
        core::String Name;
        f32 Score = -1.0f;
        NavVerdict Verdict = NavVerdict::Behind;
    };
    /// What the last directional move weighed, which the debug overlay draws and the inspector
    /// lists: where it started, which direction, every candidate of the last search, and whether
    /// an override decided it or the search wrapped around its scope.
    struct NavTrace
    {
        NavDirection Direction = NavDirection::Down;
        Rect From;
        Array<NavCandidate> Candidates;
        bool Overridden = false; ///< a nav-up (or nav-down, nav-left, nav-right) override decided it
        bool Wrapped = false;    ///< the search started again from the scope's other side
        bool Any = false;        ///< a move has happened since the GUI started
        static constexpr f32 kNoOverlapTier = 1.0e6f;
    };

    namespace detail
    {
        // Whether `node` can take focus from navigation: it is tab-focusable and enabled, it and all
        // its ancestors are shown (visible, and in the layout, so not under a page switched away),
        // and it is neither leaving nor inert.
        [[nodiscard]] inline bool IsNavigable(const Node& node)
        {
            return node.IsTabFocusable() && node.IsTreeEnabled() && node.IsTreeVisible() && !node.IsTreeInert();
        }

        // Whether a clipping ancestor hides `node`. A scroller's clip does not count, since the
        // scroller brings a focused widget into view.
        [[nodiscard]] inline bool IsClippedAway(const Node& node, const Rect& bounds)
        {
            for (const Node* n = node.GetParent(); n != nullptr; n = n->GetParent())
                if (n->ClipsChildren() && !n->ScrollsDescendantsIntoView() &&
                    (n->GetParent() == nullptr || !n->GetParent()->ScrollsDescendantsIntoView()))
                {
                    const Rect clip = n->GetScreenBounds();
                    if (bounds.x + bounds.width <= clip.x || bounds.x >= clip.x + clip.width ||
                        bounds.y + bounds.height <= clip.y || bounds.y >= clip.y + clip.height)
                        return true;
                }
            return false;
        }

        inline void CollectNavigable(Node& node, Array<Node*>& out)
        {
            if (!node.IsVisible() || !node.IsDisplayed())
                return;
            if (IsNavigable(node))
                out.PushBack(&node);
            for (usize i = 0; i < node.ChildCount(); ++i)
                CollectNavigable(*node.GetChildAt(i), out);
        }

        [[nodiscard]] inline Node* FindWidgetById(Node& node, core::StringView id)
        {
            if (const UIWidget* widget = core::Cast<UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (Node* found = FindWidgetById(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        [[nodiscard]] inline bool IsInside(const Node* node, const Node& ancestor)
        {
            for (const Node* n = node; n != nullptr; n = n->GetParent())
                if (n == &ancestor)
                    return true;
            return false;
        }

        // The score of moving from `from` to `to` in `direction`. Lower is better, and it is
        // negative when `to` is not beyond `from` in that direction. Candidates that overlap on the
        // cross axis form their own, better tier.
        [[nodiscard]] inline f32 Score(const Rect& from, const Rect& to, NavDirection direction)
        {
            const bool vertical = direction == NavDirection::Up || direction == NavDirection::Down;
            const f32 fromNear = direction == NavDirection::Down    ? from.y + from.height
                                 : direction == NavDirection::Up    ? from.y
                                 : direction == NavDirection::Right ? from.x + from.width
                                                                    : from.x;
            const f32 toNear = direction == NavDirection::Down    ? to.y
                               : direction == NavDirection::Up    ? to.y + to.height
                               : direction == NavDirection::Right ? to.x
                                                                  : to.x + to.width;
            const f32 toCenter = vertical ? to.y + to.height * 0.5f : to.x + to.width * 0.5f;
            const f32 fromCenter = vertical ? from.y + from.height * 0.5f : from.x + from.width * 0.5f;
            const bool forward = direction == NavDirection::Down || direction == NavDirection::Right;
            // Beyond means the candidate's centre is past ours and its near edge is at or past our
            // leading edge, give or take a quarter of the smaller box's size (so neighbours that
            // touch or overlap a little count). A wide field in the row above is therefore not to
            // the right of a button under it, even though its centre is; otherwise Right from a
            // Back button would jump up to a text field above it.
            if (forward ? toCenter <= fromCenter : toCenter >= fromCenter)
                return -1.0f;
            const f32 slack = 0.25f * Min(vertical ? from.height : from.width, vertical ? to.height : to.width);
            if (forward ? toNear < fromNear - slack : toNear > fromNear + slack)
                return -1.0f;
            const f32 along = Max(0.0f, forward ? toNear - fromNear : fromNear - toNear);
            const f32 crossFromLo = vertical ? from.x : from.y;
            const f32 crossFromHi = vertical ? from.x + from.width : from.y + from.height;
            const f32 crossToLo = vertical ? to.x : to.y;
            const f32 crossToHi = vertical ? to.x + to.width : to.y + to.height;
            const f32 overlap = Min(crossFromHi, crossToHi) - Max(crossFromLo, crossToLo);
            const f32 crossCenterGap = Abs((crossFromLo + crossFromHi) * 0.5f - (crossToLo + crossToHi) * 0.5f);
            constexpr f32 kNoOverlapTier = NavTrace::kNoOverlapTier; // every overlapping candidate beats every non-overlapping one
            if (overlap > 0.0f)
                return along + crossCenterGap * 0.01f;
            const f32 crossGap = -overlap;
            return kNoOverlapTier + along + crossGap * 2.0f;
        }

        // The innermost scope containing `node`, or null when there is none and the whole GUI is
        // the scope.
        [[nodiscard]] inline Node* ScopeOf(Node* node)
        {
            for (Node* n = node != nullptr ? node->GetParent() : nullptr; n != nullptr; n = n->GetParent())
                if (n->GetNavScope() != NavScope::None)
                    return n;
            return nullptr;
        }
    }

    /// Returns the best focusable widget from `from` in `direction` among the navigable widgets
    /// under `within`, or null. When wrapping, the caller passes a `fromBounds` just outside
    /// `within`'s opposite edge, so the first widget on that side is picked. `trace`, when given,
    /// receives every candidate weighed.
    [[nodiscard]] inline Node* PickInDirection(Node& within, const Node* from, const Rect& fromBounds, NavDirection direction,
                                               NavTrace* trace = nullptr)
    {
        Array<Node*> candidates;
        detail::CollectNavigable(within, candidates);
        if (trace != nullptr)
            trace->Candidates.Clear(); // keep only the last search, which made the decision
        Node* best = nullptr;
        f32 bestScore = 0.0f;
        usize bestEntry = 0;
        for (Node* candidate : candidates)
        {
            if (candidate == from || (from != nullptr && detail::IsInside(from, *candidate)))
                continue; // skip the focused widget and its ancestors (a focusable scroller around it)
            const Rect bounds = candidate->GetScreenBounds();
            const bool clipped = detail::IsClippedAway(*candidate, bounds);
            const f32 score = clipped ? -1.0f : detail::Score(fromBounds, bounds, direction);
            if (trace != nullptr)
                trace->Candidates.PushBack(NavCandidate{bounds, candidate->DebugName(), score,
                                                        clipped ? NavVerdict::Clipped : score < 0.0f ? NavVerdict::Behind : NavVerdict::Beaten});
            if (score < 0.0f)
                continue;
            if (best == nullptr || score < bestScore)
            {
                best = candidate;
                bestScore = score;
                bestEntry = trace != nullptr ? trace->Candidates.Size() - 1 : 0;
            }
        }
        if (trace != nullptr && best != nullptr)
            trace->Candidates[bestEntry].Verdict = NavVerdict::Chosen;
        return best;
    }

    /// Returns where focus lands on entering `scope` (the outermost scope a move crosses into):
    /// the widget last focused in it, else its default focus, else `picked`.
    [[nodiscard]] inline Node* EntryInto(Node& scope, Node* picked)
    {
        if (Node* remembered = scope.GetRememberedFocus();
            remembered != nullptr && detail::IsInside(remembered, scope) && detail::IsNavigable(*remembered))
            return remembered;
        if (scope.GetDefaultFocusId().Size() != 0)
            if (Node* fallback = detail::FindWidgetById(scope, scope.GetDefaultFocusId());
                fallback != nullptr && detail::IsNavigable(*fallback))
                return fallback;
        // A focus group in which nothing has been focused yet enters at its current member (a
        // checked radio button).
        if (scope.IsFocusGroup())
        {
            Array<Node*> members;
            detail::CollectNavigable(scope, members);
            for (Node* member : members)
                if (member->IsCurrentInGroup())
                    return member;
        }
        return picked;
    }

    /// Follows `focused`'s override `which` (nav-up and the others). Sets `stop` for `none`;
    /// returns the widget for an id that is found and navigable under `root`; otherwise returns
    /// null (auto, empty, or an id that is not there).
    [[nodiscard]] inline Node* FollowOverride(Node& root, const Node& focused, Node::NavOverride which, bool& stop)
    {
        stop = false;
        const core::StringView target = focused.GetNavOverride(which);
        if (target.Size() == 0 || target == core::StringView(u8"auto"))
            return nullptr;
        if (target == core::StringView(u8"none"))
        {
            stop = true;
            return nullptr;
        }
        Node* found = detail::FindWidgetById(root, target);
        return found != nullptr && detail::IsNavigable(*found) ? found : nullptr;
    }

    /// Returns the widget directional navigation moves focus to from `focused` (under `root`,
    /// which is the GUI or a modal), or null to stay. With nothing focused, it returns the root's
    /// entry (its remembered or default focus, else the first navigable widget).
    [[nodiscard]] inline Node* NavigateFrom(Node& root, Node* focused, NavDirection direction, NavTrace* trace = nullptr)
    {
        if (trace != nullptr)
        {
            *trace = NavTrace{};
            trace->Direction = direction;
            trace->Any = true;
            if (focused != nullptr)
                trace->From = focused->GetScreenBounds();
        }
        // With nothing focused, focus outside `root`, or focus on something that does not take
        // focus, go to the entry.
        if (focused == nullptr || !detail::IsInside(focused, root) || !detail::IsNavigable(*focused))
        {
            Array<Node*> all;
            detail::CollectNavigable(root, all);
            return EntryInto(root, all.IsEmpty() ? nullptr : all[0]);
        }
        // An override, for where the geometry gives the wrong answer, comes first.
        const Node::NavOverride which = direction == NavDirection::Up     ? Node::NavOverrideUp
                                        : direction == NavDirection::Down ? Node::NavOverrideDown
                                        : direction == NavDirection::Left ? Node::NavOverrideLeft
                                                                          : Node::NavOverrideRight;
        bool stop = false;
        if (Node* overridden = FollowOverride(root, *focused, which, stop))
        {
            if (trace != nullptr)
                trace->Overridden = true;
            return overridden;
        }
        if (stop)
            return nullptr;
        const Rect from = focused->GetScreenBounds();
        // Search the focused widget's scope, then move outward while each scope passes the move on
        // at its edge.
        for (Node* scope = detail::ScopeOf(focused);; scope = detail::ScopeOf(scope))
        {
            Node& within = (scope != nullptr && detail::IsInside(scope, root)) ? *scope : root;
            if (Node* picked = PickInDirection(within, focused, from, direction, trace))
            {
                // When the move crosses into a scope (one containing the pick but not the focused
                // widget), focus goes to that scope's entry.
                Node* entered = nullptr;
                for (Node* s = detail::ScopeOf(picked); s != nullptr && s != &within; s = detail::ScopeOf(s))
                    if (!detail::IsInside(focused, *s))
                        entered = s;
                return entered != nullptr ? EntryInto(*entered, picked) : picked;
            }
            if (within.GetNavScope() == NavScope::Wrap)
            {
                // Search again from just past the scope's opposite edge, along the same line.
                const Rect edge = within.GetScreenBounds();
                Rect outside = from;
                switch (direction)
                {
                case NavDirection::Down: outside.y = edge.y - from.height - 1.0f; break;
                case NavDirection::Up: outside.y = edge.y + edge.height + 1.0f; break;
                case NavDirection::Right: outside.x = edge.x - from.width - 1.0f; break;
                case NavDirection::Left: outside.x = edge.x + edge.width + 1.0f; break;
                }
                if (trace != nullptr)
                    trace->Wrapped = true;
                return PickInDirection(within, nullptr, outside, direction, trace);
            }
            if (within.GetNavScope() == NavScope::Contain || &within == &root)
                return nullptr; // the edge of a dialog, or of the GUI
            // Pass: continue with the enclosing scope.
        }
    }

    /// Returns where focus goes when a program focuses `node`: the node itself when it takes
    /// focus, else its entry (remembered, default, or first focusable widget under it), or null
    /// when nothing takes focus.
    [[nodiscard]] inline Node* FocusTargetOf(Node& node)
    {
        if (detail::IsNavigable(node))
            return &node;
        Array<Node*> all;
        detail::CollectNavigable(node, all);
        return EntryInto(node, all.IsEmpty() ? nullptr : all[0]);
    }

    /// Returns the innermost scope of kind contain around `node` (a dialog's, inside which Tab
    /// stays), or null.
    [[nodiscard]] inline Node* ContainingScopeOf(Node* node)
    {
        for (Node* scope = detail::ScopeOf(node); scope != nullptr; scope = detail::ScopeOf(scope))
            if (scope->GetNavScope() == NavScope::Contain)
                return scope;
        return nullptr;
    }

    /// Records `focused` as the remembered focus of every scope containing it.
    inline void RememberInScopes(Node& focused)
    {
        for (Node* scope = detail::ScopeOf(&focused); scope != nullptr; scope = detail::ScopeOf(scope))
            scope->RememberFocus(focused);
    }
}
