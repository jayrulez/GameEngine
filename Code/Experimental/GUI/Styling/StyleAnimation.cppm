// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_animation partition
//
// Motion (G3 of Specs/vg-and-gui-lab.md): CSS transitions and @keyframes animations over any
// property a sheet sets. A widget whose style moves gets one StyleAnimation, an action on its
// SceneRoot's clock (so a GUI's time is its own), holding the style the sheet wants (the target)
// and a track per moving property: a transition from the value showing when the property changed
// toward its new one, or a keyframe animation's track through its stops, each eased (cubic-bezier,
// steps, the named curves) and interpolated through the typed values. Each tick it applies the
// target with the tracks' values over it. It holds its widget weakly (the element table), so a
// destroyed widget ends it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:style_animation;

import foundation.core;
import foundation.fonts;
import :node;
import :ui_node;
import :element_types; // Element<T>
import :action;
import :css_values;
import :style_value;
import :style_properties;
import :style_rule;
import :style_sheet;
import :style_interpolation;
import :style_applier;
import :easing;
import :resource_provider;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    // === Declarations ===

    /// One entry of `transition`: a property (or `all`), a duration, an easing, a delay.
    struct TransitionSpec
    {
        core::String Property;
        f32 Duration = 0.0f;
        f32 Delay = 0.0f;
        Easing Timing = Easing::Ease();
    };

    /// `transition`: a comma list of `<property> <duration> [<easing>] [<delay>]` in any order
    /// past the property (the first time is the duration, the second the delay); `none` is empty.
    [[nodiscard]] inline Array<TransitionSpec> ParseTransitionList(core::StringView value)
    {
        Array<TransitionSpec> out;
        if (Trim(value) == core::StringView(u8"none"))
            return out;
        for (const core::StringView entry : SplitTopLevel(value, u8','))
        {
            TransitionSpec spec;
            spec.Property = core::String(u8"all");
            bool haveDuration = false, haveProperty = false;
            for (const core::StringView token : SplitTopLevel(entry, u8' '))
            {
                if (Optional<f32> time = ParseTime(token); time.HasValue())
                {
                    if (!haveDuration)
                    {
                        spec.Duration = time.Value();
                        haveDuration = true;
                    }
                    else
                    {
                        spec.Delay = time.Value();
                    }
                }
                else if (Optional<Easing> easing = ParseEasing(token); easing.HasValue())
                {
                    spec.Timing = easing.Value();
                }
                else if (!haveProperty)
                {
                    spec.Property = core::String(token);
                    haveProperty = true;
                }
            }
            out.PushBack(Move(spec));
        }
        return out;
    }

    enum class AnimationDirection : u8
    {
        Normal,
        Reverse,
        Alternate,
        AlternateReverse,
    };

    enum class AnimationFill : u8
    {
        None,
        Forwards,
        Backwards,
        Both,
    };

    /// One entry of `animation`.
    struct AnimationSpec
    {
        core::String Name;
        f32 Duration = 0.0f;
        f32 Delay = 0.0f;
        Easing Timing = Easing::Ease();
        f32 Iterations = 1.0f; ///< negative: infinite
        AnimationDirection Direction = AnimationDirection::Normal;
        AnimationFill Fill = AnimationFill::None;
    };

    /// `animation`: a comma list of `<name> <duration> [<easing>] [<delay>] [<iteration-count> |
    /// infinite] [<direction>] [<fill-mode>]`, keywords in any order; `none` is empty.
    [[nodiscard]] inline Array<AnimationSpec> ParseAnimationList(core::StringView value)
    {
        Array<AnimationSpec> out;
        if (Trim(value) == core::StringView(u8"none"))
            return out;
        for (const core::StringView entry : SplitTopLevel(value, u8','))
        {
            AnimationSpec spec;
            bool haveDuration = false;
            for (const core::StringView token : SplitTopLevel(entry, u8' '))
            {
                if (Optional<f32> time = ParseTime(token); time.HasValue())
                {
                    (haveDuration ? spec.Delay : spec.Duration) = time.Value();
                    haveDuration = true;
                }
                else if (token == core::StringView(u8"infinite"))
                    spec.Iterations = -1.0f;
                else if (Optional<f32> count = ParseLength(token); count.HasValue() && token.Size() > 0 &&
                                                                    (IsDigit(token[0]) || token[0] == u8'.'))
                    spec.Iterations = Max(0.0f, count.Value());
                else if (token == core::StringView(u8"normal"))
                    spec.Direction = AnimationDirection::Normal;
                else if (token == core::StringView(u8"reverse"))
                    spec.Direction = AnimationDirection::Reverse;
                else if (token == core::StringView(u8"alternate"))
                    spec.Direction = AnimationDirection::Alternate;
                else if (token == core::StringView(u8"alternate-reverse"))
                    spec.Direction = AnimationDirection::AlternateReverse;
                else if (token == core::StringView(u8"forwards"))
                    spec.Fill = AnimationFill::Forwards;
                else if (token == core::StringView(u8"backwards"))
                    spec.Fill = AnimationFill::Backwards;
                else if (token == core::StringView(u8"both"))
                    spec.Fill = AnimationFill::Both;
                else if (Optional<Easing> easing = ParseEasing(token); easing.HasValue())
                    spec.Timing = easing.Value();
                else if (spec.Name.IsEmpty())
                    spec.Name = core::String(token);
            }
            if (!spec.Name.IsEmpty())
                out.PushBack(Move(spec));
        }
        return out;
    }

    /// Whether a property's values move smoothly in a transition (`all` covers these): those the
    /// property table parses into numbers, lengths, colours, shadows, transforms, origins or layers.
    [[nodiscard]] inline bool IsTransitionable(core::StringView property)
    {
        const StylePropertySpec* spec = FindStyleProperty(property);
        if (spec == nullptr)
            return false;
        switch (spec->Grammar)
        {
        case StyleGrammar::Number:
        case StyleGrammar::Length:
        case StyleGrammar::Color:
        case StyleGrammar::Shadows:
        case StyleGrammar::Transforms:
        case StyleGrammar::Origin:
        case StyleGrammar::Layers:
            return true;
        case StyleGrammar::Keyword:
        case StyleGrammar::Text:
            return false;
        }
        return false;
    }

    // === Tracks ===

    struct AnimationKey
    {
        f32 Offset = 0.0f;
        StyleValue Value;
        Easing Timing; ///< the segment from this key to the next
    };

    /// One moving property: a transition (two keys) or a keyframe animation's track.
    struct AnimationTrack
    {
        core::String Property;
        core::String Animation; ///< the @keyframes name; empty for a transition
        Array<AnimationKey> Keys;
        f32 Duration = 0.0f;
        f32 Delay = 0.0f;
        f32 Iterations = 1.0f; ///< negative: infinite
        AnimationDirection Direction = AnimationDirection::Normal;
        AnimationFill Fill = AnimationFill::None;
        f32 Elapsed = 0.0f;

        [[nodiscard]] bool Ended() const noexcept
        {
            return Iterations >= 0.0f && Elapsed >= Delay + Duration * Iterations;
        }

        /// The value now, or false when the track shows nothing (before its delay without a
        /// backwards fill, after its end without a forwards one).
        [[nodiscard]] bool ValueNow(StyleValue& out) const
        {
            if (Keys.IsEmpty())
                return false;
            const f32 active = Elapsed - Delay;
            f32 iterationProgress = 0.0f;
            f32 iteration = 0.0f;
            if (active < 0.0f)
            {
                if (Fill != AnimationFill::Backwards && Fill != AnimationFill::Both)
                    return false;
            }
            else if (Ended() || Duration <= 0.0f)
            {
                if (Fill != AnimationFill::Forwards && Fill != AnimationFill::Both)
                    return false;
                // The end of the last iteration (a whole count ends at its full progress).
                const f32 total = Iterations < 0.0f ? 1.0f : Iterations;
                iteration = Max(0.0f, Ceil(total) - 1.0f);
                iterationProgress = total - iteration;
                if (total == 0.0f)
                {
                    iteration = 0.0f;
                    iterationProgress = 0.0f;
                }
            }
            else
            {
                const f32 cycles = active / Duration;
                iteration = Floor(cycles);
                iterationProgress = cycles - iteration;
            }
            // Direction: odd iterations of an alternating animation run backwards.
            const bool odd = static_cast<i64>(iteration) % 2 != 0;
            const bool reversed = Direction == AnimationDirection::Reverse ||
                                  (Direction == AnimationDirection::Alternate && odd) ||
                                  (Direction == AnimationDirection::AlternateReverse && !odd);
            const f32 p = reversed ? 1.0f - iterationProgress : iterationProgress;
            // The segment the progress falls in, eased by its first key's timing.
            if (p <= Keys[0].Offset)
            {
                out = Keys[0].Value;
                return true;
            }
            for (usize i = 0; i + 1 < Keys.Size(); ++i)
            {
                const AnimationKey& a = Keys[i];
                const AnimationKey& b = Keys[i + 1];
                if (p <= b.Offset)
                {
                    const f32 span = b.Offset - a.Offset;
                    const f32 local = span > 0.0f ? (p - a.Offset) / span : 1.0f;
                    out = InterpolateStyleValue(a.Value, b.Value, a.Timing.Evaluate(local));
                    return true;
                }
            }
            out = Keys[Keys.Size() - 1].Value;
            return true;
        }
    };

    /// What applying a style needs besides the style.
    struct StyleApplyContext
    {
        IResourceProvider* Resources = nullptr;
        fonts::IFontService* FontService = nullptr;
        LengthContext Lengths{};
    };

    // === The action ===

    class StyleAnimation : public Action
    {
        RTTI_OBJECT(StyleAnimation, Action)
    public:
        explicit StyleAnimation(UINode& node) : m_node(node) {}

        /// The style the sheet wants now, and what applying it needs.
        void SetTargetStyle(ResolvedStyle target, const StyleApplyContext& context)
        {
            m_targetStyle = Move(target);
            m_context = context;
        }
        [[nodiscard]] const ResolvedStyle& TargetStyle() const noexcept { return m_targetStyle; }

        /// The value a track shows for `property` now, or false when none moves it (a transition
        /// retargets from here; the target style is already the new one).
        [[nodiscard]] bool TrackValue(core::StringView property, StyleValue& out) const
        {
            for (usize i = m_tracks.Size(); i-- > 0;)
                if (m_tracks[i].Property.AsView() == property && m_tracks[i].ValueNow(out))
                    return true;
            return false;
        }

        /// A transition of `property` from `from` to `to` (replacing one already running on it).
        void StartTransition(core::StringView property, const StyleValue& from, const StyleValue& to,
                             const TransitionSpec& spec)
        {
            RemoveTracks(property, core::StringView{});
            AnimationTrack track;
            track.Property = core::String(property);
            track.Duration = spec.Duration;
            track.Delay = spec.Delay;
            track.Fill = AnimationFill::Backwards; // the old value shows through the delay
            track.Keys.PushBack(AnimationKey{0.0f, from, spec.Timing});
            track.Keys.PushBack(AnimationKey{1.0f, to, Easing::Linear()});
            m_tracks.PushBack(Move(track));
        }

        /// A keyframe animation: one track per property its stops set, each stop's declarations
        /// expanded and parsed; a property missing from the first or last stop takes the target's
        /// value there (CSS's implicit keyframes). A stop's animation-timing-function eases the
        /// segment it starts, the animation's timing the others.
        void StartKeyframes(const AnimationSpec& spec, const Keyframes& keyframes)
        {
            StopKeyframes(spec.Name.AsView());
            Array<AnimationTrack> tracks;
            const auto trackFor = [&](core::StringView property) -> AnimationTrack&
            {
                for (AnimationTrack& t : tracks)
                    if (t.Property.AsView() == property)
                        return t;
                AnimationTrack t;
                t.Property = core::String(property);
                t.Animation = spec.Name;
                t.Duration = spec.Duration;
                t.Delay = spec.Delay;
                t.Iterations = spec.Iterations;
                t.Direction = spec.Direction;
                t.Fill = spec.Fill;
                tracks.PushBack(Move(t));
                return tracks[tracks.Size() - 1];
            };
            for (const KeyframeStop& stop : keyframes.Stops)
            {
                Easing timing = spec.Timing;
                for (const StyleProperty& declared : stop.Properties)
                    if (declared.Name.AsView() == core::StringView(u8"animation-timing-function"))
                        if (Optional<Easing> e = ParseEasing(declared.Value.AsView()); e.HasValue())
                            timing = e.Value();
                for (const StyleProperty& declared : stop.Properties)
                {
                    if (declared.Name.AsView() == core::StringView(u8"animation-timing-function"))
                        continue;
                    Array<StyleLonghand> longhands;
                    if (!ExpandStyleShorthand(declared.Name.AsView(), declared.Value.AsView(), longhands))
                        continue;
                    for (const StyleLonghand& longhand : longhands)
                    {
                        const StyleValue value = ResolvedStyle::ParseLonghand(longhand.Name.AsView(), longhand.Text.AsView());
                        if (value.Kind == StyleValueKind::None)
                            continue;
                        AnimationTrack& track = trackFor(longhand.Name.AsView());
                        track.Keys.PushBack(AnimationKey{stop.Offset, value, timing});
                    }
                }
            }
            for (AnimationTrack& track : tracks)
            {
                SortKeys(track.Keys);
                const StyleValue* underlying = m_targetStyle.Value(track.Property.AsView());
                if (track.Keys[0].Offset > 0.0f && underlying != nullptr)
                    track.Keys.Insert(0, AnimationKey{0.0f, *underlying, spec.Timing});
                if (track.Keys[track.Keys.Size() - 1].Offset < 1.0f && underlying != nullptr)
                    track.Keys.PushBack(AnimationKey{1.0f, *underlying, spec.Timing});
                m_tracks.PushBack(Move(track));
            }
            m_animations.PushBack(spec.Name);
        }

        void StopKeyframes(core::StringView name)
        {
            RemoveTracks(core::StringView{}, name);
            for (usize i = 0; i < m_animations.Size(); ++i)
            {
                if (m_animations[i].AsView() == name)
                {
                    m_animations.RemoveAt(i);
                    break;
                }
            }
        }
        [[nodiscard]] bool RunsKeyframes(core::StringView name) const
        {
            for (const core::String& n : m_animations)
                if (n.AsView() == name)
                    return true;
            return false;
        }
        [[nodiscard]] const Array<core::String>& KeyframeAnimations() const noexcept { return m_animations; }
        [[nodiscard]] usize TrackCount() const noexcept { return m_tracks.Size(); }

        /// Applies the target with every track's value now over it.
        void ApplyNow()
        {
            UINode* node = m_node.Get();
            if (node == nullptr)
            {
                m_done = true;
                return;
            }
            ResolvedStyle effective = m_targetStyle;
            for (const AnimationTrack& track : m_tracks)
            {
                StyleValue value;
                if (track.ValueNow(value))
                    effective.Override(track.Property.AsView(), value);
                else
                    ResetToInitial(effective, track.Property.AsView()); // its delay or after its end
            }
            for (const core::String& property : m_resets)
                ResetToInitial(effective, property.AsView());
            m_resets.Clear();
            ApplyStyle(*node, effective, m_context.Resources, m_context.FontService, m_context.Lengths);
        }

        // === Action ===
        void Start() override { ApplyNow(); }
        void Stop() override { m_done = true; }
        void Update(core::Duration elapsed) override
        {
            if (m_done)
                return;
            const f32 seconds = elapsed.AsSecondsF();
            for (AnimationTrack& track : m_tracks)
                track.Elapsed += seconds;
            ApplyNow();
            // Ended tracks that hold nothing go (a forwards fill keeps its track, showing its end).
            for (usize i = 0; i < m_tracks.Size();)
            {
                const AnimationTrack& t = m_tracks[i];
                const bool holds = t.Fill == AnimationFill::Forwards || t.Fill == AnimationFill::Both;
                if (t.Ended() && (!holds || t.Animation.IsEmpty()))
                {
                    m_resets.PushBack(t.Property);
                    m_tracks.RemoveAt(i);
                }
                else
                {
                    ++i;
                }
            }
            if (!m_resets.IsEmpty())
                ApplyNow(); // what the ended tracks moved returns to the target, or its initial value
        }
        [[nodiscard]] bool IsDone() override { return m_done || m_tracks.IsEmpty(); }
        [[nodiscard]] f32 GetCurrentProgress() override { return 0.0f; }
        [[nodiscard]] core::Duration GetTotalTime() override { return core::Duration{}; }

    private:
        // A property the target does not set takes its initial value (when the table gives one).
        void ResetToInitial(ResolvedStyle& effective, core::StringView property) const
        {
            if (m_targetStyle.Value(property) != nullptr)
                return;
            const StylePropertySpec* spec = FindStyleProperty(property);
            if (spec == nullptr || spec->Initial == nullptr)
                return;
            StyleValue initial;
            if (ParseStyleValue(spec->Grammar, core::StringView(spec->Initial), initial))
                effective.Override(property, initial);
        }

        static void SortKeys(Array<AnimationKey>& keys)
        {
            for (usize a = 1; a < keys.Size(); ++a)
            {
                AnimationKey key = keys[a];
                usize b = a;
                while (b > 0 && keys[b - 1].Offset > key.Offset)
                {
                    keys[b] = keys[b - 1];
                    --b;
                }
                keys[b] = Move(key);
            }
        }

        // Removes the tracks of `property` (any, when empty) belonging to `animation` (transitions
        // when empty, unless `property` is empty too).
        void RemoveTracks(core::StringView property, core::StringView animation)
        {
            for (usize i = 0; i < m_tracks.Size();)
            {
                const AnimationTrack& t = m_tracks[i];
                const bool match = property.IsEmpty() ? t.Animation.AsView() == animation
                                                      : (t.Property.AsView() == property && t.Animation.IsEmpty());
                if (match)
                {
                    m_resets.PushBack(t.Property);
                    m_tracks.RemoveAt(i);
                }
                else
                {
                    ++i;
                }
            }
        }

        Element<UINode> m_node;
        ResolvedStyle m_targetStyle;
        StyleApplyContext m_context;
        Array<AnimationTrack> m_tracks;
        Array<core::String> m_animations;
        Array<core::String> m_resets; // properties whose tracks just went, to return on the next apply
        bool m_done = false;
    };

    RTTI_DEFINE_OBJECT(StyleAnimation, "rtti::gui")
}
