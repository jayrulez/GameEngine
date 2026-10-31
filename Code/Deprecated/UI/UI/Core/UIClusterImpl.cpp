// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - module implementation unit for foundation.ui.
//
// Holds the two styling methods that call into View (StyleSelector::Matches, StyleSheet::Resolve).
// They cannot live in the :view interface partition without making :style_selector/:style_sheet depend
// back on :view (a module-partition cycle), and they cannot live in their own interface partitions
// without a forward-declared View being incomplete. An implementation unit sees the whole module
// (it implicitly imports the primary interface) and is outside the interface dependency graph.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

module foundation.ui;

using namespace foundation::core;

namespace foundation::ui
{
    // === View cluster method bodies (kept out of the :view interface partition to slim its BMI) ===

    void View::Invalidate()
    {
        // The safe default: any mutation may have changed geometry, so the host re-lays-out
        // before redrawing. Visual-only producers use InvalidateVisual to skip that pass.
        m_needsRedraw = true;
        if (Context != nullptr)
        {
            Context->MarkNeedsLayout();
        }
    }

    void View::InvalidateVisual()
    {
        m_needsRedraw = true;
        if (Context != nullptr)
        {
            Context->MarkNeedsRedraw();
        }
    }

    RootView* View::Root() const
    {
        View* view = const_cast<View*>(this);
        while (view != nullptr)
        {
            if (RootView* root = Cast<RootView>(view))
            {
                return root;
            }
            view = view->Parent;
        }
        return nullptr;
    }

    bool View::IsEffectivelyVisible() const
    {
        const View* v = this;
        while (v != nullptr)
        {
            if (v->Visibility != VisibilityValue::Visible)
            {
                return false;
            }
            if (Cast<RootView>(const_cast<View*>(v)) != nullptr)
            {
                return true;
            }
            v = v->Parent;
        }
        return false;
    }

    void View::InvalidateStyle()
    {
        // The cache stays valid but stale: the context generation bump makes the next lookup
        // rebuild it, and the rebuild reads the old values first so a transition can start
        // from where the view is. Detached views rebuild on every access anyway.
        if (Context != nullptr)
        {
            Context->InvalidateStyles();
        }
        else
        {
            m_styleCache.valid = false;
        }
        Invalidate();
    }

    void View::InvalidateStyleSheets()
    {
        m_styleCache.valid = false;
        if (Context != nullptr)
        {
            Context->BumpSheetEpoch();
        }
        Invalidate();
    }

    // === Transitions ===

    namespace
    {
        [[nodiscard]] f32 TransitionProgress(f32 elapsed, f32 duration, f32 delay, const TimingFunction& easing)
        {
            if (duration <= 0.0f)
            {
                return 1.0f;
            }
            const f32 t = Clamp((elapsed - delay) / duration, 0.0f, 1.0f);
            return easing.Evaluate(t);
        }
    }

    usize View::ActiveTransitionCount() const noexcept
    {
        return m_transitionState ? m_transitionState->Active.Size() : 0u;
    }

    usize View::RunningAnimationCount() const noexcept
    {
        usize running = 0;
        for (const RunningAnimation& animation : m_animations)
        {
            running += animation.Finished ? 0u : 1u;
        }
        return running;
    }

    const KeyframesRule* View::FindKeyframes(StringView name) const
    {
        for (const View* v = this; v != nullptr; v = v->Parent)
        {
            if (v->m_localStyleSheet)
            {
                if (const KeyframesRule* found = v->m_localStyleSheet->FindKeyframes(name))
                {
                    return found;
                }
            }
        }
        if (Context != nullptr)
        {
            if (const StyleSheet* sheet = Context->GetStyleSheet())
            {
                return sheet->FindKeyframes(name);
            }
        }
        return nullptr;
    }

    void View::DeliverAnimationEvent(bool ended, const String& name)
    {
        RefPtr<View> self(this);
        auto fire = [self, ended, name]()
        { (ended ? self->OnAnimationEnd : self->OnAnimationIteration).Invoke(self.Get(), name.AsView()); };
        if (Context != nullptr)
        {
            Context->MutationQueueRef().QueueAction(Move(fire));
        }
        else
        {
            fire();
        }
    }

    void View::SyncAnimations()
    {
        // Read from the cache just built, never through ComputeStyle: a view outside a context
        // keeps no valid cache, and would rebuild it here, and sync again.
        StyleValue value;
        if (const StyleRule* winner = m_styleCache.winners[static_cast<usize>(StyleProperty::Animation)])
        {
            if (const Optional<StyleValue> raw = winner->GetValue(StyleProperty::Animation); raw.HasValue())
            {
                value = raw.Value();
            }
        }
        const AnimationList* list = value.AsAnimations();
        if ((list == nullptr || list->Specs.IsEmpty()) && m_animations.IsEmpty())
        {
            return;
        }
        Array<RunningAnimation> next;
        if (list != nullptr)
        {
            for (const AnimationSpec& spec : list->Specs)
            {
                const KeyframesRule* keyframes = FindKeyframes(spec.Name.AsView());
                if (keyframes == nullptr)
                {
                    continue; // no such @keyframes: nothing plays
                }
                RunningAnimation running;
                running.Spec = spec;
                running.Keyframes = RefPtr<KeyframesRule>(const_cast<KeyframesRule*>(keyframes));
                // The same name over the same keyframes carries on where it was.
                for (const RunningAnimation& old : m_animations)
                {
                    if (old.Spec.Name == spec.Name && old.Keyframes.Get() == keyframes)
                    {
                        running.Elapsed = old.Elapsed;
                        running.Finished = old.Finished;
                    }
                }
                next.PushBack(Move(running));
            }
        }
        bool changed = next.Size() != m_animations.Size();
        for (usize i = 0; !changed && i < next.Size(); ++i)
        {
            changed = next[i].Keyframes.Get() != m_animations[i].Keyframes.Get() ||
                      next[i].Spec.Name != m_animations[i].Spec.Name;
        }
        m_animations = Move(next);
        if (RunningAnimationCount() > 0)
        {
            RegisterTransitioning();
        }
        if (changed)
        {
            InvalidateVisual();
        }
    }

    Optional<StyleValue> View::AnimatedValue(StyleProperty prop, const StyleValue& base)
    {
        for (usize a = m_animations.Size(); a-- > 0;)
        {
            const RunningAnimation& running = m_animations[a];
            const AnimationSpec& spec = running.Spec;
            const KeyframesRule& keyframes = *running.Keyframes;
            if (!keyframes.Animates(prop))
            {
                continue;
            }
            // Where in its run the animation is: an iteration and the way through it.
            const f32 local = running.Elapsed - spec.Delay;
            const bool infinite = spec.Iterations < 0.0f;
            const f32 duration = Max(spec.Duration, 0.0f);
            const f32 total = infinite ? kFloatMax : duration * spec.Iterations;
            f32 iteration = 0.0f;
            f32 progress = 0.0f;
            if (local < 0.0f)
            {
                if (spec.Fill != AnimationFill::Backwards && spec.Fill != AnimationFill::Both)
                {
                    continue;
                }
            }
            else if (!infinite && local >= total)
            {
                if (spec.Fill != AnimationFill::Forwards && spec.Fill != AnimationFill::Both)
                {
                    continue;
                }
                // The end of the last iteration: a count of 2.5 ends half way through the third.
                const f32 whole = Floor(spec.Iterations);
                iteration = spec.Iterations > whole ? whole : Max(0.0f, whole - 1.0f);
                progress = spec.Iterations > whole ? spec.Iterations - whole : 1.0f;
            }
            else if (duration > 0.0f)
            {
                iteration = Floor(local / duration);
                progress = local / duration - iteration;
            }
            else
            {
                progress = 1.0f;
            }
            const bool odd = static_cast<i64>(iteration) % 2 == 1;
            const bool reversed = spec.Direction == AnimationDirection::Reverse ||
                                  (spec.Direction == AnimationDirection::Alternate && odd) ||
                                  (spec.Direction == AnimationDirection::AlternateReverse && !odd);
            const f32 p = reversed ? 1.0f - progress : progress;

            // The frames either side of p that declare the property; a missing first or last
            // frame is the cascade's value, as CSS fills it in.
            f32 fromOffset = 0.0f;
            StyleValue from = base;
            f32 toOffset = 1.0f;
            StyleValue to = base;
            for (const KeyframesRule::Frame& frame : keyframes.Frames)
            {
                const Optional<StyleValue> raw = frame.Declarations->GetValue(prop);
                if (!raw.HasValue())
                {
                    continue;
                }
                if (frame.Offset <= p)
                {
                    fromOffset = frame.Offset;
                    from = ResolveKeywords(prop, raw.Value(), 0);
                }
                else
                {
                    toOffset = frame.Offset;
                    to = ResolveKeywords(prop, raw.Value(), 0);
                    break;
                }
            }
            if (toOffset <= fromOffset)
            {
                return from;
            }
            const f32 t = spec.Easing.Evaluate((p - fromOffset) / (toOffset - fromOffset));
            if (from.IsNone() || to.IsNone() || !StyleValuesInterpolable(from, to))
            {
                return t < 0.5f ? from : to;
            }
            return LerpStyleValue(from, to, t);
        }
        return {};
    }

    bool View::IsTransitioning() const noexcept
    {
        return m_transitionState &&
               (!m_transitionState->Active.IsEmpty() || m_transitionState->StateBlendActive);
    }

    void View::RegisterTransitioning()
    {
        if (!m_transitionRegistered && Context != nullptr)
        {
            Context->RegisterTransitioning(this);
            m_transitionRegistered = true;
        }
    }

    void View::ClearTransitions()
    {
        m_transitionState.Reset();
        m_transitionRegistered = false;
    }

    bool View::AdvanceTransitions(f32 deltaTime)
    {
        // The running animations advance on the same tick; each frame they redraw the view, or
        // lay it out when one animates a property that moves geometry.
        bool animating = false;
        bool animated = false;
        bool animationLayoutDamage = false;
        for (RunningAnimation& running : m_animations)
        {
            if (running.Finished)
            {
                continue;
            }
            animated = true;
            const f32 before = running.Elapsed;
            running.Elapsed += deltaTime;
            const AnimationSpec& spec = running.Spec;
            const f32 duration = Max(spec.Duration, 0.0f);
            if (spec.Iterations >= 0.0f && running.Elapsed >= spec.Delay + duration * spec.Iterations)
            {
                running.Finished = true;
                DeliverAnimationEvent(true, running.Spec.Name);
            }
            else if (duration > 0.0f && running.Elapsed > spec.Delay &&
                     Floor(Max(before - spec.Delay, 0.0f) / duration) < Floor((running.Elapsed - spec.Delay) / duration))
            {
                DeliverAnimationEvent(false, running.Spec.Name); // a new iteration began
            }
            animating = animating || !running.Finished;
            for (const KeyframesRule::Frame& frame : running.Keyframes->Frames)
            {
                for (usize e = 0; e < frame.Declarations->PropertyCount(); ++e)
                {
                    animationLayoutDamage =
                        animationLayoutDamage || !IsVisualOnlyStyleProperty(frame.Declarations->GetProperty(e).Prop);
                }
            }
        }
        if (animated)
        {
            if (animationLayoutDamage)
            {
                Invalidate();
            }
            else
            {
                InvalidateVisual();
            }
        }
        if (!m_transitionState)
        {
            m_transitionRegistered = animating;
            return animating;
        }
        TransitionState& st = *m_transitionState;
        bool layoutDamage = false;
        for (usize i = st.Active.Size(); i-- > 0;)
        {
            StyleTransition& tr = st.Active[i];
            tr.Elapsed += deltaTime;
            if (!IsVisualOnlyStyleProperty(tr.Property))
            {
                layoutDamage = true;
            }
            if (tr.Elapsed >= tr.Delay + tr.Duration)
            {
                st.Active.RemoveAtSwap(i); // the cascade value takes over (== To)
            }
        }
        if (st.StateBlendActive)
        {
            st.BlendElapsed += deltaTime;
            if (st.BlendElapsed >= st.BlendDelay + st.BlendDuration)
            {
                st.StateBlendActive = false;
            }
        }
        if (layoutDamage)
        {
            Invalidate();
        }
        else
        {
            InvalidateVisual();
        }
        const bool active = !st.Active.IsEmpty() || st.StateBlendActive || animating;
        if (!active)
        {
            m_transitionRegistered = false;
        }
        return active;
    }

    DrawBlend View::CurrentDrawBlend() const
    {
        DrawBlend blend;
        if (!m_transitionState)
        {
            return blend;
        }
        const TransitionState& st = *m_transitionState;
        if (st.StateBlendActive)
        {
            blend.StateActive = true;
            blend.FromState = st.BlendFrom;
            blend.ToState = st.BlendTo;
            blend.StateT = TransitionProgress(st.BlendElapsed, st.BlendDuration, st.BlendDelay, st.BlendEasing);
        }
        for (const StyleTransition& tr : st.Active)
        {
            if (tr.Property == StyleProperty::Background && tr.From.GetKind() == StyleValue::Kind::Drawable)
            {
                blend.FromDrawable = tr.From.AsDrawable();
                blend.ToDrawable = tr.To.AsDrawable();
                blend.DrawableT = TransitionProgress(tr.Elapsed, tr.Duration, tr.Delay, tr.Easing);
            }
        }
        return blend;
    }

    void View::BeginTransitions(const StyleRule* const* oldWinners, ControlState oldState,
                                ControlState newState)
    {
        const StyleValue specValue = ComputeStyle(StyleProperty::Transition);
        const TransitionList* specs = specValue.AsTransitions();
        if (specs == nullptr || specs->Specs.IsEmpty())
        {
            return;
        }
        bool started = false;
        for (usize p = 0; p < static_cast<usize>(StyleProperty::COUNT); ++p)
        {
            const StyleProperty prop = static_cast<StyleProperty>(p);
            if (!IsAnimatableStyleProperty(prop))
            {
                continue;
            }
            const StyleRule* oldRule = oldWinners[p];
            const StyleRule* newRule = m_styleCache.winners[p];
            if (oldRule == newRule)
            {
                continue; // the same rule under the same sheets: the same value
            }
            const TransitionSpec* spec = specs->Find(prop);
            if (spec == nullptr || spec->Duration <= 0.0f)
            {
                continue;
            }
            // From: a running entry's current value (a re-trigger never snaps), else the old
            // rule's value, else what the view inherited before (an inheritable property
            // that had no rule of its own).
            StyleTransition* entry = nullptr;
            if (m_transitionState)
            {
                for (StyleTransition& tr : m_transitionState->Active)
                {
                    if (tr.Property == prop)
                    {
                        entry = &tr;
                    }
                }
            }
            StyleValue from;
            if (entry != nullptr)
            {
                from = LerpStyleValue(entry->From, entry->To,
                                      TransitionProgress(entry->Elapsed, entry->Duration, entry->Delay, entry->Easing));
            }
            else if (oldRule != nullptr)
            {
                if (const Optional<StyleValue> raw = oldRule->GetValue(prop); raw.HasValue())
                {
                    from = ResolveKeywords(prop, raw.Value(), 0);
                }
            }
            else if (IsInheritableStyle(prop) && Parent != nullptr)
            {
                from = Parent->ResolveStyle(prop);
            }
            if (from.IsNone())
            {
                continue;
            }
            const StyleValue to = ComputeStyle(prop);
            if (to.IsNone() || StyleValueEquivalent(from, to) || !StyleValuesInterpolable(from, to))
            {
                continue;
            }
            if (prop == StyleProperty::Background && from.GetKind() != StyleValue::Kind::Drawable)
            {
                continue;
            }
            if (!m_transitionState)
            {
                m_transitionState = MakeUnique<TransitionState>(MemoryAllocator());
            }
            if (entry == nullptr)
            {
                m_transitionState->Active.PushBack(StyleTransition{});
                entry = &m_transitionState->Active[m_transitionState->Active.Size() - 1];
            }
            entry->Property = prop;
            entry->From = from;
            entry->To = to;
            entry->Elapsed = 0.0f;
            entry->Duration = spec->Duration;
            entry->Delay = spec->Delay;
            entry->Easing = spec->Easing;
            started = true;
        }
        if (newState != oldState)
        {
            // State-aware drawables pick their colors inside Draw, invisible to the cascade:
            // cross-fade the whole background between the two states under the `background`
            // (or `all`) entry.
            if (const TransitionSpec* spec = specs->Find(StyleProperty::Background);
                spec != nullptr && spec->Duration > 0.0f)
            {
                if (!m_transitionState)
                {
                    m_transitionState = MakeUnique<TransitionState>(MemoryAllocator());
                }
                TransitionState& st = *m_transitionState;
                ControlState from = oldState;
                if (st.StateBlendActive)
                {
                    // Mid-blend retarget: start from whichever state is currently more visible.
                    const f32 t = TransitionProgress(st.BlendElapsed, st.BlendDuration, st.BlendDelay, st.BlendEasing);
                    from = t < 0.5f ? st.BlendFrom : st.BlendTo;
                }
                st.StateBlendActive = from != newState;
                st.BlendFrom = from;
                st.BlendTo = newState;
                st.BlendElapsed = 0.0f;
                st.BlendDuration = spec->Duration;
                st.BlendDelay = spec->Delay;
                st.BlendEasing = spec->Easing;
                started = started || st.StateBlendActive;
            }
        }
        if (started)
        {
            RegisterTransitioning();
        }
    }

    void UIContext::SetStyleSheet(RefPtr<StyleSheet> sheet)
    {
        // The theme is data, motion included: a sheet declares its own `transition` rules
        // (every shipped theme puts one on View). The engine prepends nothing and keeps no
        // list of control types.
        m_styleSheet = Move(sheet);
        BumpSheetEpoch();
    }

    const View::StyleCache& View::EnsureStyleCache()
    {
        const ControlState state = GetControlState();
        const u32 generation = Context != nullptr ? Context->StyleGeneration() : 0u;
        // The chain: the sheets whose versions this cache keys on, and the local sheets it
        // collects from (outermost ancestor first, so a nearer one comes later and wins).
        View* chain[64];
        usize depth = 0;
        u32 chainVersion = 0;
        if (Context != nullptr)
        {
            if (const StyleSheet* sheet = Context->GetStyleSheet())
            {
                chainVersion += sheet->Version();
            }
        }
        for (View* anc = this; anc != nullptr && depth < 64; anc = anc->Parent)
        {
            if (anc->m_localStyleSheet)
            {
                chain[depth++] = anc;
                chainVersion += anc->m_localStyleSheet->Version();
            }
        }
        if (m_inlineSheet)
        {
            chainVersion += m_inlineSheet->Version();
        }
        const u32 sheetEpoch = Context != nullptr ? Context->SheetEpoch() : 0u;
        // Without a context there is no generation to key on (class edits on this view would
        // go unseen), so the cache is rebuilt every time. Only tests run views that way.
        if (m_styleCache.valid && Context != nullptr && m_styleCache.generation == generation &&
            m_styleCache.chainVersion == chainVersion && m_styleCache.state == state &&
            m_styleCache.sheetEpoch == sheetEpoch)
        {
            return m_styleCache;
        }

        // Transition trigger: remember the old winners, but only when their rule pointers are
        // known to be alive, which holds when the sheet epoch and the summed sheet versions
        // are unchanged and only the generation or the state moved. A sheet swap or a rule
        // edit rebuilds without animating. BeginTransitions then compares winners per
        // property: the same rule under the same sheets is the same value, so nothing is
        // resolved unless a winner changed, which keeps a theme-wide `View { transition }`
        // cheap.
        const bool canTransition = m_styleCache.valid && Context != nullptr &&
                                   m_styleCache.sheetEpoch == sheetEpoch &&
                                   m_styleCache.chainVersion == chainVersion;
        const ControlState oldState = m_styleCache.state;
        const StyleRule* oldWinners[static_cast<usize>(StyleProperty::COUNT)];
        if (canTransition)
        {
            for (usize p = 0; p < static_cast<usize>(StyleProperty::COUNT); ++p)
            {
                oldWinners[p] = m_styleCache.winners[p];
            }
        }
        else if (m_transitionState)
        {
            // The rules moved under a running transition (theme swap, rule edit): its targets
            // are stale, so it ends here and the new cascade value applies at once. The
            // context's tick de-lists the view on its next pass.
            m_transitionState->Active.Clear();
            m_transitionState->StateBlendActive = false;
        }

        StyleCache& cache = m_styleCache;
        cache.rules.Clear();
        // Ascending cascade order: the context sheet, then the local sheets from the outermost
        // ancestor inward (nearer wins), then this view's inline sheet (wins over everything).
        if (Context != nullptr)
        {
            if (const StyleSheet* sheet = Context->GetStyleSheet())
            {
                sheet->CollectMatching(*this, state, StringView{}, cache.rules);
            }
        }
        for (usize i = depth; i-- > 0;)
        {
            chain[i]->m_localStyleSheet->CollectMatching(*this, state, StringView{}, cache.rules);
        }
        if (m_inlineSheet)
        {
            m_inlineSheet->CollectMatching(*this, state, StringView{}, cache.rules);
        }

        for (usize p = 0; p < static_cast<usize>(StyleProperty::COUNT); ++p)
        {
            cache.winners[p] = nullptr;
        }
        // Last declaration wins: walk from the highest-priority rule down, first fill wins.
        usize filled = 0;
        for (usize i = cache.rules.Size(); i-- > 0 && filled < static_cast<usize>(StyleProperty::COUNT);)
        {
            const StyleRule* rule = cache.rules[i];
            for (usize e = 0; e < rule->PropertyCount(); ++e)
            {
                const usize p = static_cast<usize>(rule->GetProperty(e).Prop);
                if (p < static_cast<usize>(StyleProperty::COUNT) && cache.winners[p] == nullptr)
                {
                    cache.winners[p] = rule;
                    ++filled;
                }
            }
        }
        cache.valid = true;
        cache.generation = generation;
        cache.chainVersion = chainVersion;
        cache.sheetEpoch = sheetEpoch;
        cache.state = state;
        if (canTransition)
        {
            BeginTransitions(oldWinners, oldState, state);
        }
        SyncAnimations();
        return cache;
    }

    StyleValue View::RawStyleValue(StyleProperty prop)
    {
        const StyleCache& cache = EnsureStyleCache();
        const usize p = static_cast<usize>(prop);
        if (p >= static_cast<usize>(StyleProperty::COUNT) || cache.winners[p] == nullptr)
        {
            return StyleValue::None();
        }
        if (Optional<StyleValue> v = cache.winners[p]->GetValue(prop); v.HasValue())
        {
            return v.Value();
        }
        return StyleValue::None();
    }

    StyleValue View::ResolveVariableValue(const StyleValue& reference, i32 depth)
    {
        const VariableReference* ref = reference.Variable();
        if (ref == nullptr || depth > 8)
        {
            return StyleValue::None();
        }
        StyleValue value = CustomProperty(ref->Name.AsView());
        if (value.IsNone())
        {
            value = VariableFallback(*ref);
        }
        if (value.GetKind() == StyleValue::Kind::Variable)
        {
            return ResolveVariableValue(value, depth + 1);
        }
        return value;
    }

    StyleValue View::ResolveKeywords(StyleProperty prop, const StyleValue& raw, i32 depth)
    {
        switch (raw.GetKind())
        {
        case StyleValue::Kind::Inherit:
            return Parent != nullptr ? Parent->ResolveStyle(prop) : StyleValue::None();
        case StyleValue::Kind::Initial:
            return StyleValue::None();
        case StyleValue::Kind::Variable:
        {
            StyleValue value = ResolveVariableValue(raw, depth);
            if (value.NeedsResolution() && depth <= 8)
            {
                return ResolveKeywords(prop, value, depth + 1);
            }
            return value;
        }
        default:
            return raw;
        }
    }

    StyleValue View::ResolveStyle(StyleProperty prop)
    {
        StyleValue value = ComputeStyle(prop);
        if (m_transitionState)
        {
            for (const StyleTransition& tr : m_transitionState->Active)
            {
                if (tr.Property != prop)
                {
                    continue;
                }
                if (tr.To.GetKind() == StyleValue::Kind::Drawable)
                {
                    return value; // the draw path cross-fades (CurrentDrawBlend)
                }
                return LerpStyleValue(tr.From, tr.To,
                                      TransitionProgress(tr.Elapsed, tr.Duration, tr.Delay, tr.Easing));
            }
        }
        // An animation's values over the cascade's (a transition, above, wins over both, as in
        // CSS).
        if (!m_animations.IsEmpty() && prop != StyleProperty::Animation)
        {
            if (Optional<StyleValue> animated = AnimatedValue(prop, value); animated.HasValue())
            {
                return animated.Value();
            }
        }
        return value;
    }

    StyleValue View::ComputeStyle(StyleProperty prop)
    {
        const StyleValue raw = RawStyleValue(prop);
        if (!raw.IsNone())
        {
            const StyleValue value = ResolveKeywords(prop, raw, 0);
            if (!value.IsNone() || raw.GetKind() == StyleValue::Kind::Initial)
            {
                return value;
            }
            // An unset variable with no fallback behaves like "unset": fall through to inherit.
        }
        if (IsInheritableStyle(prop) && Parent != nullptr)
        {
            return Parent->ResolveStyle(prop);
        }
        return StyleValue::None();
    }

    StyleValue View::CustomProperty(StringView name)
    {
        const u64 hash = HashText(name);
        for (View* v = this; v != nullptr; v = v->Parent)
        {
            const StyleCache& cache = v->EnsureStyleCache();
            for (usize i = cache.rules.Size(); i-- > 0;)
            {
                if (const StyleValue* value = cache.rules[i]->FindCustom(hash, name))
                {
                    if (value->GetKind() == StyleValue::Kind::Variable)
                    {
                        return v->ResolveVariableValue(*value, 1);
                    }
                    return *value;
                }
            }
        }
        return StyleValue::None();
    }

    f32 View::ResolveStyleLength(StyleProperty prop, f32 referenceSize, f32 defaultVal)
    {
        const StyleValue value = ResolveStyle(prop);
        if (Optional<f32> f = value.AsFloat(); f.HasValue())
        {
            return f.Value();
        }
        if (Optional<Unit> length = value.AsLength(); length.HasValue())
        {
            // em: relative to this view's computed font size, which for font-size itself is the
            // parent's (as in CSS), so `font-size: 1.5em` compounds down the tree.
            const f32 fontSize =
                prop == StyleProperty::FontSize
                    ? (Parent != nullptr ? Parent->ResolveStyleLength(StyleProperty::FontSize, 0.0f, 16.0f) : 16.0f)
                    : ResolveStyleLength(StyleProperty::FontSize, 0.0f, 16.0f);
            RootView* root = Root();
            const f32 dpiScale = (root != nullptr) ? Max(root->DpiScale, 0.01f) : 1.0f;
            return length.Value().Resolve(dpiScale, referenceSize, fontSize);
        }
        return defaultVal;
    }

    StyleValue View::ResolvePartStyle(StringView part, StyleProperty prop, ControlState partState)
    {
        // Parts are uncached (per-part state varies per draw); same cascade order as the element.
        Array<const StyleRule*> rules;
        if (Context != nullptr)
        {
            if (const StyleSheet* sheet = Context->GetStyleSheet())
            {
                sheet->CollectMatching(*this, partState, part, rules);
            }
        }
        View* chain[64];
        usize depth = 0;
        for (View* anc = this; anc != nullptr && depth < 64; anc = anc->Parent)
        {
            if (anc->m_localStyleSheet)
            {
                chain[depth++] = anc;
            }
        }
        for (usize i = depth; i-- > 0;)
        {
            chain[i]->m_localStyleSheet->CollectMatching(*this, partState, part, rules);
        }
        if (m_inlineSheet)
        {
            m_inlineSheet->CollectMatching(*this, partState, part, rules);
        }
        for (usize i = rules.Size(); i-- > 0;)
        {
            if (Optional<StyleValue> v = rules[i]->GetValue(prop); v.HasValue())
            {
                return ResolveKeywords(prop, v.Value(), 0);
            }
        }
        return StyleValue::None();
    }

    String View::ResolveStyleFontFamily()
    {
        // Hold the StyleValue in a named local: AsString() borrows a view into it (dangles otherwise).
        StyleValue family = ResolveStyle(StyleProperty::FontFamily);
        if (Optional<StringView> s = family.AsString(); s.HasValue())
        {
            return String(s.Value());
        }
        if (Context != nullptr && Context->FontService() != nullptr)
        {
            return String(Context->FontService()->DefaultFontFamily());
        }
        return String{};
    }

    String View::ResolveStyleFontFamily(StringView instanceOverride)
    {
        if (instanceOverride.Size() > 0)
        {
            return String(instanceOverride);
        }
        return ResolveStyleFontFamily();
    }

    void View::QueueRemove()
    {
        if (Context == nullptr || IsPendingDeletion)
        {
            return;
        }
        IsPendingDeletion = true;
        // Strong capture: the queue co-owns the view until the action drains, so a synchronous
        // RemoveView between queue and drain can never leave the lambda holding a freed pointer,
        // and the flag reset below is always safe (the ref drops at lambda end).
        RefPtr<View> self(this);
        Context->MutationQueueRef().QueueAction(
            [self]()
            {
                if (self->Parent != nullptr)
                {
                    if (ViewGroup* pg = Cast<ViewGroup>(self->Parent))
                    {
                        pg->RemoveView(self.Get(), false);
                    }
                }
                self->IsPendingDeletion = false;
            });
    }

    void View::QueueDestroy()
    {
        if (Context == nullptr || IsPendingDeletion)
        {
            return;
        }
        IsPendingDeletion = true;
        RefPtr<View> self(this); // strong capture (see QueueRemove); also makes the reset safe
        Context->MutationQueueRef().QueueAction(
            [self]()
            {
                if (self->Parent != nullptr)
                {
                    if (ViewGroup* pg = Cast<ViewGroup>(self->Parent))
                    {
                        pg->RemoveView(self.Get(), true);
                    }
                }
                // Reset so a view kept alive by an outside ref (destroy is advisory under RefPtr
                // ownership) is not permanently immune to future queued removals.
                self->IsPendingDeletion = false;
            });
    }

    f32 ViewGroup::RootDpiScale(RootView* root) { return root != nullptr ? root->DpiScale : 1.0f; }

    void View::RefreshStyleTransform()
    {
        const StyleCache& cache = EnsureStyleCache();
        if (cache.winners[static_cast<usize>(StyleProperty::Transform)] == nullptr && ActiveTransitionCount() == 0 &&
            m_animations.IsEmpty())
        {
            m_styleMatrix = Transform2D{};
            return;
        }
        const Optional<StyleTransform> styled = ResolveStyle(StyleProperty::Transform).AsTransform();
        if (!styled.HasValue() || styled.Value().IsIdentity())
        {
            m_styleMatrix = Transform2D{};
            return;
        }
        RootView* root = Root();
        const f32 dpi = root != nullptr ? Max(root->DpiScale, 0.01f) : 1.0f;
        const Float2 size{Width(), Height()};
        const Optional<StylePoint> origin = ResolveStyle(StyleProperty::TransformOrigin).AsPoint();
        const StylePoint pivot = origin.HasValue() ? origin.Value() : StylePoint{};
        const f32 ox = pivot.X.Resolve(dpi, size.x, 0.0f);
        const f32 oy = pivot.Y.Resolve(dpi, size.y, 0.0f);
        Transform2D m;
        m.Translate(ox, oy);
        m.Combine(styled.Value().Compose(size, dpi));
        m.Translate(-ox, -oy);
        m_styleMatrix = m;
    }

    f32 View::DeviceUnitScale() const
    {
        f32 scale = 1.0f;
        const View* top = this;
        for (const View* above = Parent; above != nullptr; above = above->Parent)
        {
            // A rotation, a skew or an uneven scale has no single grid to round to, so it adds no factor.
            f32 uniform = 1.0f;
            if (above->TransformMatrix().UniformScale(uniform))
            {
                scale *= uniform;
            }
            top = above;
        }
        const RootView* root = Cast<RootView>(const_cast<View*>(top));
        return Max((root != nullptr ? root->DpiScale : 1.0f) * scale, 0.01f);
    }

    // Recomputes the effective LayoutStyle from the inline value and the cascade (see
    // View::Layout() in View.cppm).
    void View::RefreshEffectiveLayout()
    {
        // The effective copy marks sheet-filled fields declared too: consumers ask "is Right
        // set?" of the effective layout and must not care where the value came from. The
        // inline m_layout keeps its own flags.
        LayoutStyle e = m_layout;
        m_styledOverflowHidden = false;
        const StyleCache& cache = EnsureStyleCache();
        // One cache lookup, then only the properties a rule actually sets pay for keyword /
        // variable resolution (RefreshEffectiveLayout runs at the top of every Measure).
        const auto set = [&cache](StyleProperty prop) {
            return cache.winners[static_cast<usize>(prop)] != nullptr;
        };
        const auto sizeSpec = [&](StyleProperty prop, Declared<SizeSpec>& field) {
            if (field.IsDeclared() || !set(prop))
            {
                return;
            }
            const StyleValue v = ResolveStyle(prop);
            if (Optional<StringView> word = v.AsString(); word.HasValue())
            {
                const StringView w = word.Value();
                if (w == StringView(u8"match") || w == StringView(u8"match-parent") ||
                    w == StringView(u8"fill") || w == StringView(u8"stretch"))
                {
                    field = SizeSpec::Match();
                }
                else if (w == StringView(u8"wrap") || w == StringView(u8"wrap-content") ||
                         w == StringView(u8"auto"))
                {
                    field = SizeSpec::Wrap();
                }
            }
            else if (Optional<f32> f = v.AsFloat(); f.HasValue())
            {
                field = SizeSpec::Fixed(Unit::Dp(f.Value()));
            }
            else if (Optional<Unit> u = v.AsLength(); u.HasValue())
            {
                field = SizeSpec::Fixed(u.Value());
            }
        };
        const auto unit = [&](StyleProperty prop, Declared<Unit>& field) {
            if (field.IsDeclared() || !set(prop))
            {
                return;
            }
            const StyleValue v = ResolveStyle(prop);
            if (Optional<f32> f = v.AsFloat(); f.HasValue())
            {
                field = Unit::Dp(f.Value());
            }
            else if (Optional<Unit> u = v.AsLength(); u.HasValue())
            {
                field = u.Value();
            }
        };
        const auto number = [&](StyleProperty prop, Declared<f32>& field) {
            if (field.IsDeclared() || !set(prop))
            {
                return;
            }
            // Insets/flex factors: absolute lengths (dp/px/em) resolve here; a percent inset
            // has no reference box at refresh time and contributes 0 (unlike CSS).
            field = ResolveStyleLength(prop, 0.0f, field.value);
        };
        sizeSpec(StyleProperty::Width, e.Width);
        sizeSpec(StyleProperty::Height, e.Height);
        if (!e.Margin.IsDeclared() && set(StyleProperty::Margin))
        {
            if (Optional<Thickness> t = ResolveStyle(StyleProperty::Margin).AsThickness(); t.HasValue())
            {
                e.Margin = t.Value();
            }
        }
        unit(StyleProperty::MinWidth, e.MinWidth);
        unit(StyleProperty::MinHeight, e.MinHeight);
        unit(StyleProperty::MaxWidth, e.MaxWidth);
        unit(StyleProperty::MaxHeight, e.MaxHeight);
        number(StyleProperty::Top, e.Top);
        number(StyleProperty::Right, e.Right);
        number(StyleProperty::Bottom, e.Bottom);
        number(StyleProperty::Left, e.Left);
        number(StyleProperty::FlexGrow, e.FlexGrow);
        number(StyleProperty::FlexShrink, e.FlexShrink);
        unit(StyleProperty::FlexBasis, e.FlexBasis);
        if (!e.ZIndex.IsDeclared() && set(StyleProperty::ZIndex))
        {
            e.ZIndex = static_cast<i32>(ResolveStyleFloat(StyleProperty::ZIndex, 0.0f));
        }
        if (!e.Position.IsDeclared() && set(StyleProperty::Position))
        {
            const String word = ResolveStyleString(StyleProperty::Position);
            e.Position = word == StringView(u8"absolute") ? Position::Absolute : Position::Static;
        }
        if (!e.AlignSelf.HasValue() && set(StyleProperty::AlignSelf))
        {
            const String word = ResolveStyleString(StyleProperty::AlignSelf);
            const StringView w = word.AsView();
            if (w == StringView(u8"start") || w == StringView(u8"flex-start"))
                e.AlignSelf = Align::Start;
            else if (w == StringView(u8"end") || w == StringView(u8"flex-end"))
                e.AlignSelf = Align::End;
            else if (w == StringView(u8"center"))
                e.AlignSelf = Align::Center;
            else if (w == StringView(u8"stretch"))
                e.AlignSelf = Align::Stretch;
            else if (w == StringView(u8"baseline"))
                e.AlignSelf = Align::Baseline;
        }
        if (set(StyleProperty::Overflow))
        {
            const String word = ResolveStyleString(StyleProperty::Overflow);
            m_styledOverflowHidden = word == StringView(u8"hidden");
        }
        m_effectiveLayout = e;
    }

    void View::Measure(BoxConstraints c)
    {
        m_measureRequest = c;
        if (Context == nullptr)
        {
            MeasureUncached(c);
            return;
        }
        const u64 generation = Context->MeasureGeneration();
        if (m_measureGeneration != generation)
        {
            m_measureGeneration = generation;
            m_measureAnswerCount = 0;
            m_measureAnswerNext = 0;
        }
        else
        {
            for (u32 i = 0; i < m_measureAnswerCount; ++i)
            {
                if (m_measureAnswers[i].constraints == c)
                {
                    MeasuredSize = m_measureAnswers[i].size;
                    m_measureStale = !(c == m_measuredFor);
                    return;
                }
            }
        }
        MeasureUncached(c);
        m_measureAnswers[m_measureAnswerNext] = MeasureAnswer{c, MeasuredSize};
        m_measureAnswerNext = (m_measureAnswerNext + 1) % kMeasureAnswers;
        m_measureAnswerCount = Min(m_measureAnswerCount + 1, kMeasureAnswers);
    }

    void View::MeasureUncached(BoxConstraints c)
    {
        m_measuredFor = c;
        m_measureStale = false;
        // Effective layout first: this view's (its own spec/margin below) and its children's
        // (the container's OnMeasure reads child->Layout() before measuring each child).
        RefreshEffectiveLayout();
        RefreshChildEffectiveLayouts();

        const BoxMetrics metrics = ResolveBoxMetrics();
        BoxConstraints box = c.Deflate(metrics.Margin);

        const LayoutStyle& layout = m_effectiveLayout;
        const SizeSpec widthSpec = layout.Width;
        const SizeSpec heightSpec = layout.Height;
        const bool clampsW = layout.MinWidth.Value() != Unit{} || layout.MaxWidth.Value() != Unit{};
        const bool clampsH = layout.MinHeight.Value() != Unit{} || layout.MaxHeight.Value() != Unit{};
        if (widthSpec.kind == SizeSpec::Kind::Fixed || heightSpec.kind == SizeSpec::Kind::Fixed ||
            clampsW || clampsH)
        {
            RootView* root = Root();
            const f32 dpiScale = (root != nullptr) ? Max(root->DpiScale, 0.01f) : 1.0f;
            // Percent resolves against the containing box on the same axis (the incoming
            // constraint's max, after margin; 0 when unbounded), em against the computed font
            // size. Both are only computed when a component needs them.
            const bool relative = widthSpec.fixedSize.IsRelative() || heightSpec.fixedSize.IsRelative() ||
                                  layout.MinWidth->IsRelative() || layout.MaxWidth->IsRelative() ||
                                  layout.MinHeight->IsRelative() || layout.MaxHeight->IsRelative();
            const f32 fontSize = relative ? ResolveStyleLength(StyleProperty::FontSize, 0.0f, 16.0f) : 0.0f;
            const f32 referenceW = BoxConstraints::IsBounded(box.MaxWidth) ? box.MaxWidth : 0.0f;
            const f32 referenceH = BoxConstraints::IsBounded(box.MaxHeight) ? box.MaxHeight : 0.0f;
            if (widthSpec.kind == SizeSpec::Kind::Fixed)
            {
                const f32 w = Max(0.0f, widthSpec.ResolveFixed(dpiScale, referenceW, fontSize));
                box.MinWidth = w;
                box.MaxWidth = w;
            }
            if (heightSpec.kind == SizeSpec::Kind::Fixed)
            {
                const f32 h = Max(0.0f, heightSpec.ResolveFixed(dpiScale, referenceH, fontSize));
                box.MinHeight = h;
                box.MaxHeight = h;
            }
            // min/max clamp the constraint band (a Fixed size included); max wins over min
            // when they cross, like CSS.
            if (clampsW)
            {
                if (layout.MinWidth.Value() != Unit{})
                {
                    const f32 minW = Max(0.0f, layout.MinWidth->Resolve(dpiScale, referenceW, fontSize));
                    box.MinWidth = Max(box.MinWidth, minW);
                    box.MaxWidth = Max(box.MaxWidth, minW);
                }
                if (layout.MaxWidth.Value() != Unit{})
                {
                    const f32 maxW = Max(0.0f, layout.MaxWidth->Resolve(dpiScale, referenceW, fontSize));
                    box.MaxWidth = Min(box.MaxWidth, maxW);
                    box.MinWidth = Min(box.MinWidth, maxW);
                }
            }
            if (clampsH)
            {
                if (layout.MinHeight.Value() != Unit{})
                {
                    const f32 minH = Max(0.0f, layout.MinHeight->Resolve(dpiScale, referenceH, fontSize));
                    box.MinHeight = Max(box.MinHeight, minH);
                    box.MaxHeight = Max(box.MaxHeight, minH);
                }
                if (layout.MaxHeight.Value() != Unit{})
                {
                    const f32 maxH = Max(0.0f, layout.MaxHeight->Resolve(dpiScale, referenceH, fontSize));
                    box.MaxHeight = Min(box.MaxHeight, maxH);
                    box.MinHeight = Min(box.MinHeight, maxH);
                }
            }
        }

        const Thickness chrome = metrics.Chrome();
        const Float2 content = OnMeasureContent(box.Deflate(chrome));
        if (content.x >= 0.0f)
        {
            MeasuredSize = Float2{box.ConstrainWidth(content.x + chrome.TotalHorizontal()),
                                  box.ConstrainHeight(content.y + chrome.TotalVertical())};
        }
        else
        {
            OnMeasure(box); // the older hook: the control handles its own chrome
        }
        // Absolute children never feed MeasuredSize; they are measured against the content
        // box this view just settled on.
        MeasureAbsoluteChildren(Max(0.0f, MeasuredSize.x - chrome.TotalHorizontal()),
                                Max(0.0f, MeasuredSize.y - chrome.TotalVertical()));
    }

    // The base arrange (margin inset + device-grid rounding). Rounds
    // edges independently (x and x+w each snap, width = snapped difference) so adjacent
    // rounded boxes stay gapless; local-grid rounding composes to the global grid because
    // every ancestor rounds too (integer sums stay integers, at fractional scales multiples
    // of 1/dpi stay multiples).
    void View::Layout(f32 x, f32 y, f32 width, f32 height)
    {
        // The parent lays this view out by its last answer; when that came from the measure store
        // for other constraints than the subtree last held, measure the subtree for it, once, so
        // every child lays out by what that request gives it (View::Measure).
        if (m_measureStale)
        {
            MeasureUncached(m_measureRequest);
        }
        const Thickness margin = m_effectiveLayout.Margin;
        f32 bx = x + margin.Left;
        f32 by = y + margin.Top;
        f32 bw = Max(0.0f, width - margin.TotalHorizontal());
        f32 bh = Max(0.0f, height - margin.TotalVertical());

        // Rounded to device pixels at the scale this box is drawn at: the root's DPI, times the
        // uniform scales of the views above it (a game canvas scaled to its reference
        // resolution), so a border at a scale of 0.48 lands on whole pixels too.
        const f32 dpi = DeviceUnitScale();
        const f32 x1 = Round((bx + bw) * dpi) / dpi;
        const f32 y1 = Round((by + bh) * dpi) / dpi;
        bx = Round(bx * dpi) / dpi;
        by = Round(by * dpi) / dpi;
        bw = Max(0.0f, x1 - bx);
        bh = Max(0.0f, y1 - by);

        Bounds = Rectangle{bx, by, bw, bh};
        OnLayout(bx, by, bw, bh);
        LayoutAbsoluteChildren();
    }

    // Lazily create the RootView's PopupLayer (kept as the last child). Defined here because the
    // :popup_layer type is incomplete in the :view partition (module cycle) but complete in this impl unit.
    PopupLayer* RootView::GetPopupLayer()
    {
        if (!m_popupLayer)
        {
            RefPtr<PopupLayer> pl = MakeRef<PopupLayer>(MemoryAllocator());
            m_popupLayer = RefPtr<ViewGroup>(pl.Get()); // upcast + ref
            ViewGroup::AddView(pl.Get()); // base add (bypasses RootView's keep-last override)
        }
        return Cast<PopupLayer>(m_popupLayer.Get());
    }

    ViewGroup* ViewGroup::AddView(View* child)
    {
        // Mutating the tree while it is being drawn corrupts the in-progress child walk.
        // Mutation during layout is fine (virtualization realizes rows there). Defer draw-phase
        // mutations via UIContext::MutationQueueRef().QueueAction.
        DIAGNOSTIC_ASSERT(Context == nullptr || Context->CurrentPhase() != UIContext::Phase::Drawing);
        if (child == nullptr || child == this)
        {
            return this;
        }
        for (const RefPtr<View>& c : m_children)
        {
            if (c.Get() == child)
            {
                return this;
            }
        }

        if (child->Parent != nullptr)
        {
            if (ViewGroup* oldParent = Cast<ViewGroup>(child->Parent))
            {
                oldParent->RemoveView(child, false);
            }
        }

        child->Parent = this;
        if (Context != nullptr)
        {
            Context->AttachView(child);
        }
        else
        {
            child->Context = nullptr;
        }
        m_children.PushBack(RefPtr<View>(child));
        Invalidate();
        return this;
    }

    ViewGroup::~ViewGroup()
    {
        // A destroyed group must not leave a child pointing back at it. RemoveView clears the
        // back-pointer for children it detaches, but on destruction the children array is simply
        // released, so a child still held elsewhere (e.g. a persistent editor view reused
        // across PropertyGrid rebuilds) would keep a dangling Parent, and the next AddView would
        // dereference freed memory when it tries to detach from the old parent. Null it here.
        // Also detach from the context, as RemoveView/RemoveAllViews do: a still-attached
        // subtree destroyed by dropping its owning RefPtr would otherwise leave the UIContext
        // registry and the focus/hover/animation tables holding freed View*.
        for (const RefPtr<View>& child : m_children)
        {
            if (!child)
            {
                continue;
            }
            if (child->Context != nullptr)
            {
                child->Context->DetachView(child.Get());
            }
            if (child->Parent == this)
            {
                child->Parent = nullptr;
            }
        }
    }

    void ViewGroup::RemoveView(View* child, bool deleteChild)
    {
        (void)deleteChild; // RefPtr ownership makes this advisory: dropping the tree ref frees it.
        DIAGNOSTIC_ASSERT(Context == nullptr ||
                          Context->CurrentPhase() != UIContext::Phase::Drawing); // defer via MutationQueue
        if (child == nullptr)
        {
            return;
        }
        for (usize i = 0; i < m_children.Size(); ++i)
        {
            if (m_children[i].Get() != child)
            {
                continue;
            }
            RefPtr<View> keepAlive = m_children[i]; // hold across Detach + Parent clear
            if (child->Context != nullptr)
            {
                child->Context->DetachView(child);
            }
            child->Parent = nullptr;
            m_children.RemoveAt(i);
            Invalidate();
            return;
        }
    }

    void ViewGroup::RemoveAllViews(bool deleteChildren)
    {
        (void)deleteChildren;
        DIAGNOSTIC_ASSERT(Context == nullptr ||
                          Context->CurrentPhase() != UIContext::Phase::Drawing); // defer via MutationQueue
        for (const RefPtr<View>& child : m_children)
        {
            if (child->Context != nullptr)
            {
                child->Context->DetachView(child.Get());
            }
            child->Parent = nullptr;
        }
        m_children.Clear();
        Invalidate();
    }

    void ViewGroup::InsertView(View* child, usize index)
    {
        DIAGNOSTIC_ASSERT(Context == nullptr ||
                          Context->CurrentPhase() != UIContext::Phase::Drawing); // defer via MutationQueue
        if (child == nullptr || child == this)
        {
            return;
        }
        for (const RefPtr<View>& c : m_children)
        {
            if (c.Get() == child)
            {
                return;
            }
        }

        if (child->Parent != nullptr)
        {
            if (ViewGroup* oldParent = Cast<ViewGroup>(child->Parent))
            {
                oldParent->RemoveView(child, false);
            }
        }

        child->Parent = this;
        if (Context != nullptr)
        {
            Context->AttachView(child);
        }
        else
        {
            child->Context = nullptr;
        }

        const usize clamped = index > m_children.Size() ? m_children.Size() : index;
        m_children.Insert(clamped, RefPtr<View>(child));
        Invalidate();
    }

    void ViewGroup::MoveView(View* child, usize index)
    {
        if (child == nullptr || m_children.IsEmpty())
        {
            return;
        }
        if (Context != nullptr)
        {
            Context->InvalidateStyles(); // :first-child / :last-child follow the order
        }
        usize current = m_children.Size();
        for (usize i = 0; i < m_children.Size(); ++i)
        {
            if (m_children[i].Get() == child)
            {
                current = i;
                break;
            }
        }
        if (current == m_children.Size())
        {
            return;
        } // not a child of this group
        const usize target = index >= m_children.Size() ? m_children.Size() - 1 : index;
        if (target == current)
        {
            return;
        }
        RefPtr<View> keepAlive = m_children[current];
        m_children.RemoveAt(current);
        // Inserting at `target` after the removal lands the child at final index `target`
        // regardless of direction (the removal already shifted trailing entries left).
        m_children.Insert(target, Move(keepAlive));
        Invalidate();
    }

    void MutationQueue::QueueDelete(View* view)
    {
        if (view == nullptr || view->IsPendingDeletion)
        {
            return;
        }
        view->IsPendingDeletion = true;
        RefPtr<View> keep(view); // strong capture (see View::QueueRemove); the reset is then safe
        QueueAction(
            [keep]()
            {
                if (keep->Parent != nullptr)
                {
                    if (ViewGroup* pg = Cast<ViewGroup>(keep->Parent))
                    {
                        pg->RemoveView(keep.Get(), false);
                    }
                }
                keep->IsPendingDeletion = false;
            });
    }

    namespace
    {
        [[nodiscard]] bool CompoundMatches(const SelectorCompound& c, const View& view,
                                           ControlState state)
        {
            if (c.UnknownType)
            {
                return false;
            }
            if (c.ViewType != nullptr && !IsDerivedFrom(view.GetType(), c.ViewType))
            {
                return false;
            }
            for (const String& cls : c.StyleClasses)
            {
                if (!view.HasClass(cls.AsView()))
                {
                    return false;
                }
            }
            if (c.Id.HasValue() && view.Name != c.Id.Value())
            {
                return false;
            }
            if (c.State.HasValue())
            {
                const ControlState required = c.State.Value();
                if (required != ControlState::Normal && !HasAllFlags(state, required)) // every flag of the compound
                {
                    return false;
                }
            }
            if (c.Structural != StructuralMatch::None)
            {
                const ViewGroup* parent = Cast<ViewGroup>(view.Parent);
                if (HasStructural(c.Structural, StructuralMatch::FirstChild) &&
                    (parent == nullptr || parent->ChildCount() == 0 || parent->GetChildAt(0) != &view))
                {
                    return false;
                }
                if (HasStructural(c.Structural, StructuralMatch::LastChild) &&
                    (parent == nullptr || parent->ChildCount() == 0 ||
                     parent->GetChildAt(parent->ChildCount() - 1) != &view))
                {
                    return false;
                }
                if (HasStructural(c.Structural, StructuralMatch::Empty))
                {
                    const ViewGroup* self = Cast<ViewGroup>(&view);
                    if (self != nullptr && self->ChildCount() != 0)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        // Right-to-left with backtracking: a descendant step may match any ancestor, and the
        // steps beyond it must still match from there.
        [[nodiscard]] bool AncestorsMatch(const Array<SelectorAncestor>& steps, usize index,
                                          const View& from)
        {
            if (index >= steps.Size())
            {
                return true;
            }
            const SelectorAncestor& step = steps[index];
            if (step.DirectParent)
            {
                const View* parent = from.Parent;
                return parent != nullptr &&
                       CompoundMatches(step.Compound, *parent, parent->GetControlState()) &&
                       AncestorsMatch(steps, index + 1, *parent);
            }
            for (const View* anc = from.Parent; anc != nullptr; anc = anc->Parent)
            {
                if (CompoundMatches(step.Compound, *anc, anc->GetControlState()) &&
                    AncestorsMatch(steps, index + 1, *anc))
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool StyleSelector::Matches(const View& view, ControlState state,
                                StringView pseudoElement) const
    {
        if (PseudoElement.HasValue())
        {
            if (pseudoElement.Size() == 0u || PseudoElement.Value().AsView() != pseudoElement)
            {
                return false;
            }
        }
        else if (pseudoElement.Size() != 0u)
        {
            return false;
        }
        if (UnknownType)
        {
            return false;
        }
        if (ViewType != nullptr && !IsDerivedFrom(view.GetType(), ViewType))
        {
            return false;
        }
        for (const String& cls : StyleClasses)
        {
            if (!view.HasClass(cls.AsView()))
            {
                return false;
            }
        }
        if (Id.HasValue() && view.Name != Id.Value())
        {
            return false;
        }
        if (State.HasValue())
        {
            const ControlState required = State.Value();
            if (required != ControlState::Normal && !HasAllFlags(state, required)) // every flag of the compound
            {
                return false;
            }
        }
        if (Structural != StructuralMatch::None)
        {
            SelectorCompound structuralOnly;
            structuralOnly.Structural = Structural;
            if (!CompoundMatches(structuralOnly, view, state))
            {
                return false;
            }
        }
        return Ancestors.IsEmpty() || AncestorsMatch(Ancestors, 0, view);
    }

    void StyleSheet::CollectMatching(const View& view, ControlState state, StringView pseudo,
                                     Array<const StyleRule*>& out) const
    {
        // Gather in source order, then stable-sort by specificity: equal specificity keeps
        // declaration order, so the later rule ends up later (and wins).
        const usize first = out.Size();
        for (const RefPtr<StyleRule>& rule : m_rules)
        {
            if (rule->Selector.Matches(view, state, pseudo))
            {
                out.PushBack(rule.Get());
            }
        }
        for (usize i = first + 1; i < out.Size(); ++i)
        {
            const StyleRule* key = out[i];
            const i32 keySpecificity = key->Selector.Specificity();
            usize j = i;
            while (j > first && out[j - 1]->Selector.Specificity() > keySpecificity)
            {
                out[j] = out[j - 1];
                --j;
            }
            out[j] = key;
        }
    }

    StyleValue StyleSheet::Resolve(const View& view, StyleProperty prop) const
    {
        return ResolveMatching(view, view.GetControlState(), StringView{}, prop);
    }
}
