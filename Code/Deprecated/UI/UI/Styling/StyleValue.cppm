// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :style_value partition
//
// A tagged value stored in a StyleRule: Color, Float, Thickness, Drawable, Bool, String, None and
// the richer kinds below. It is a kind plus separate storage members rather than a union, so the
// Drawable is a RefPtr<Drawable> and the string an owned String, and copying, moving or destroying
// a StyleValue needs no manual reference counting.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:style_value;

import foundation.core;
import :thickness;
import :unit;
import :drawable;
import :style_property;
import :transform2d; // StyleTransform::Compose

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui
{
    class StyleValue;

    /// One `box-shadow` (or text effect): the CSS tuple. Offsets, blur and spread are logical units (dp)
    /// at resolve time; the VG draws it as one blurred rounded-rect (VGContext::FillBoxShadow). A
    /// box-shadow value holds a list of them (StyleValue::AsShadows).
    struct BoxShadow
    {
        f32 OffsetX = 0.0f;
        f32 OffsetY = 0.0f;
        f32 Blur = 0.0f;
        f32 Spread = 0.0f;
        core::Color Color{0.0f, 0.0f, 0.0f, 0.5f};
        bool Inset = false;

        [[nodiscard]] constexpr bool operator==(const BoxShadow&) const noexcept = default;
    };

    /// One function of a sheet's `transform`. Translate moves by X and Y (lengths: dp, or a percent of
    /// the view's own size); Scale scales by A and B; Rotate turns by A degrees (clockwise); Skew
    /// slants by A and B degrees (CSS's skew(ax, ay)); Matrix is M (a, b, c, d, tx, ty), what
    /// interpolating two lists of different functions gives.
    enum class TransformFunctionKind : u8
    {
        Translate,
        Scale,
        Rotate,
        Skew,
        Matrix,
    };
    struct TransformFunction
    {
        TransformFunctionKind Kind = TransformFunctionKind::Translate;
        Unit X{};
        Unit Y{};
        f32 A = 0.0f;
        f32 B = 0.0f;
        f32 M[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

        /// The function that does nothing, of this one's kind (CSS pads `none` with them).
        [[nodiscard]] TransformFunction Identity() const noexcept
        {
            TransformFunction f;
            f.Kind = Kind;
            f.A = Kind == TransformFunctionKind::Scale ? 1.0f : 0.0f;
            f.B = Kind == TransformFunctionKind::Scale ? 1.0f : 0.0f;
            return f;
        }
        [[nodiscard]] bool operator==(const TransformFunction& o) const noexcept
        {
            if (Kind != o.Kind || !(X == o.X) || !(Y == o.Y) || A != o.A || B != o.B)
                return false;
            for (usize i = 0; i < 6; ++i)
                if (M[i] != o.M[i])
                    return false;
            return true;
        }
    };

    /// A sheet's `transform`: its functions in written order, composed as CSS composes them (the
    /// last written applies to the view first), about the view's `transform-origin`. It is drawn
    /// over the view's own Transform field.
    struct StyleTransform
    {
        static constexpr usize kMaxFunctions = 8;
        TransformFunction Functions[kMaxFunctions]{};
        u8 Count = 0;

        [[nodiscard]] bool IsIdentity() const noexcept { return Count == 0; }
        /// Appends a function; a list written with more keeps its first ones.
        void Push(const TransformFunction& f) noexcept
        {
            if (Count < kMaxFunctions)
                Functions[Count++] = f;
        }
        [[nodiscard]] Span<const TransformFunction> List() const noexcept
        {
            return Span<const TransformFunction>(Functions, Count);
        }
        /// The functions as one matrix, for a view of `size` (a translate's percentages) at
        /// `dpiScale`. The origin is the caller's.
        [[nodiscard]] Transform2D Compose(core::Float2 size, f32 dpiScale) const noexcept
        {
            Transform2D m;
            for (usize i = 0; i < Count; ++i)
            {
                const TransformFunction& f = Functions[i];
                switch (f.Kind)
                {
                case TransformFunctionKind::Translate:
                    m.Translate(f.X.Resolve(dpiScale, size.x, 0.0f), f.Y.Resolve(dpiScale, size.y, 0.0f));
                    break;
                case TransformFunctionKind::Scale:
                    m.Scale(f.A, f.B);
                    break;
                case TransformFunctionKind::Rotate:
                    m.Rotate(f.A * kDegToRad);
                    break;
                case TransformFunctionKind::Skew:
                    m.Skew(f.A * kDegToRad, f.B * kDegToRad);
                    break;
                case TransformFunctionKind::Matrix:
                    m.Combine(Transform2D{f.M[0], f.M[1], f.M[2], f.M[3], f.M[4], f.M[5]});
                    break;
                }
            }
            return m;
        }
        [[nodiscard]] bool operator==(const StyleTransform& o) const noexcept
        {
            if (Count != o.Count)
                return false;
            for (usize i = 0; i < Count; ++i)
                if (!(Functions[i] == o.Functions[i]))
                    return false;
            return true;
        }
    };

    /// A point as two lengths, x then y (CSS's transform-origin: dp, or a percent of the view).
    struct StylePoint
    {
        Unit X = Unit::Percent(50.0f);
        Unit Y = Unit::Percent(50.0f);
        [[nodiscard]] bool operator==(const StylePoint& o) const noexcept { return X == o.X && Y == o.Y; }
    };

    /// A sheet's `filter` (or `backdrop-filter`): a blur's sigma in dp, and the colour filters
    /// a layer's composite applies, a saturation (0 grey, 1 unchanged) and a brightness factor.
    struct StyleFilter
    {
        f32 Blur = 0.0f;
        f32 Saturation = 1.0f;
        f32 Brightness = 1.0f;

        [[nodiscard]] constexpr bool FiltersColour() const noexcept { return Saturation != 1.0f || Brightness != 1.0f; }
        [[nodiscard]] constexpr bool operator==(const StyleFilter&) const noexcept = default;
    };

    /// A CSS timing function: the cubic Bezier from (0, 0) to (1, 1) through (X1, Y1) and (X2, Y2)
    /// that `cubic-bezier()` writes (the named easings are presets of it), or `steps(n)`, which
    /// holds each of n levels and jumps at a step's end (or, with StepsJumpStart, its start).
    struct TimingFunction
    {
        f32 X1 = 0.25f;
        f32 Y1 = 0.1f;
        f32 X2 = 0.25f;
        f32 Y2 = 1.0f;
        u16 Steps = 0; ///< 0: the curve
        bool StepsJumpStart = false;

        [[nodiscard]] static constexpr TimingFunction CubicBezier(f32 x1, f32 y1, f32 x2, f32 y2) noexcept
        {
            TimingFunction f;
            f.X1 = x1;
            f.Y1 = y1;
            f.X2 = x2;
            f.Y2 = y2;
            return f;
        }
        [[nodiscard]] static constexpr TimingFunction Linear() noexcept { return CubicBezier(0.0f, 0.0f, 1.0f, 1.0f); }
        [[nodiscard]] static constexpr TimingFunction Ease() noexcept { return CubicBezier(0.25f, 0.1f, 0.25f, 1.0f); }
        [[nodiscard]] static constexpr TimingFunction EaseIn() noexcept { return CubicBezier(0.42f, 0.0f, 1.0f, 1.0f); }
        [[nodiscard]] static constexpr TimingFunction EaseOut() noexcept { return CubicBezier(0.0f, 0.0f, 0.58f, 1.0f); }
        [[nodiscard]] static constexpr TimingFunction EaseInOut() noexcept
        {
            return CubicBezier(0.42f, 0.0f, 0.58f, 1.0f);
        }
        [[nodiscard]] static constexpr TimingFunction StepsOf(u16 count, bool jumpStart = false) noexcept
        {
            TimingFunction f = Linear();
            f.Steps = count;
            f.StepsJumpStart = jumpStart;
            return f;
        }

        /// The eased progress at `t` (0 to 1): the curve's y where its x is t. Y may leave 0 to 1
        /// for a curve that overshoots (a back-out bounce).
        [[nodiscard]] f32 Evaluate(f32 t) const noexcept
        {
            t = Clamp(t, 0.0f, 1.0f);
            if (Steps > 0)
            {
                const f32 n = static_cast<f32>(Steps);
                const f32 step = StepsJumpStart ? Ceil(t * n) : Floor(t * n);
                return t >= 1.0f ? 1.0f : Min(step, n) / n;
            }
            if (X1 == Y1 && X2 == Y2)
            {
                return t; // the straight line
            }
            // Solve x(u) = t for the curve's parameter u (Newton, then bisection if it stalls),
            // then read y(u).
            const auto at = [](f32 a, f32 b, f32 u)
            {
                const f32 v = 1.0f - u;
                return 3.0f * v * v * u * a + 3.0f * v * u * u * b + u * u * u;
            };
            const auto slope = [](f32 a, f32 b, f32 u)
            {
                const f32 v = 1.0f - u;
                return 3.0f * v * v * a + 6.0f * v * u * (b - a) + 3.0f * u * u * (1.0f - b);
            };
            f32 u = t;
            for (i32 i = 0; i < 8; ++i)
            {
                const f32 error = at(X1, X2, u) - t;
                if (Abs(error) < 1e-5f)
                {
                    return at(Y1, Y2, u);
                }
                const f32 d = slope(X1, X2, u);
                if (Abs(d) < 1e-6f)
                {
                    break;
                }
                u -= error / d;
            }
            f32 lo = 0.0f;
            f32 hi = 1.0f;
            u = t;
            for (i32 i = 0; i < 32; ++i)
            {
                const f32 x = at(X1, X2, u);
                if (Abs(x - t) < 1e-5f)
                {
                    break;
                }
                (x < t ? lo : hi) = u;
                u = (lo + hi) * 0.5f;
            }
            return at(Y1, Y2, u);
        }

        [[nodiscard]] constexpr bool operator==(const TimingFunction&) const noexcept = default;
    };

    /// One entry of a `transition:` list. `Property == StyleProperty::COUNT` means `all`.
    struct TransitionSpec
    {
        StyleProperty Property = StyleProperty::COUNT;
        /// Seconds.
        f32 Duration = 0.0f;
        f32 Delay = 0.0f;
        TimingFunction Easing = TimingFunction::Ease();

        [[nodiscard]] constexpr bool operator==(const TransitionSpec&) const noexcept = default;
    };

    /// The parsed `transition:` list, shared by every StyleValue copy that carries it. An
    /// empty list is `transition: none` (it still wins the cascade over a UA `all`).
    class TransitionList : public Object
    {
        RTTI_OBJECT(TransitionList, Object)
    public:
        Array<TransitionSpec> Specs;

        TransitionList() = default;

        /// The entry governing `prop`: the last one naming it or `all` (in CSS, later entries
        /// override earlier ones), or null when nothing covers it.
        [[nodiscard]] const TransitionSpec* Find(StyleProperty prop) const noexcept
        {
            const TransitionSpec* found = nullptr;
            for (const TransitionSpec& spec : Specs)
            {
                if (spec.Property == prop || spec.Property == StyleProperty::COUNT)
                {
                    found = &spec;
                }
            }
            return found;
        }
    };

    /// Which way an animation's iterations run (CSS's animation-direction).
    enum class AnimationDirection : u8
    {
        Normal,
        Reverse,
        Alternate,        ///< forwards, then backwards, ...
        AlternateReverse, ///< backwards, then forwards, ...
    };

    /// What an animation's values do outside its run (CSS's animation-fill-mode): none leaves
    /// the cascade's, forwards holds the last frame after it ends, backwards shows the first during
    /// its delay, both does both.
    enum class AnimationFill : u8
    {
        None,
        Forwards,
        Backwards,
        Both,
    };

    /// One entry of an `animation:` list: the @keyframes it plays and how.
    struct AnimationSpec
    {
        String Name;
        f32 Duration = 0.0f; ///< seconds, one iteration
        f32 Delay = 0.0f;    ///< seconds
        TimingFunction Easing = TimingFunction::Ease();
        f32 Iterations = 1.0f; ///< negative: infinite
        AnimationDirection Direction = AnimationDirection::Normal;
        AnimationFill Fill = AnimationFill::None;
    };

    /// The parsed `animation:` list, shared by every StyleValue copy that carries it. An empty
    /// list is `animation: none`.
    class AnimationList : public Object
    {
        RTTI_OBJECT(AnimationList, Object)
    public:
        Array<AnimationSpec> Specs;

        AnimationList() = default;
    };

    /// A `var(--name, fallback)` reference: resolved against the view's custom properties at
    /// compute time, never at parse time, since the value depends on where the view sits. Shared
    /// by every copy of the StyleValue that carries it; immutable once built.
    class VariableReference : public Object
    {
        RTTI_OBJECT(VariableReference, Object)
    public:
        String Name;     ///< Including the leading `--`.
        u64 NameHash = 0; ///< HashText(Name)
        /// Used when the variable is unset (may itself be a reference: nested fallback).
        /// Owned here through a pointer to keep StyleValue's own definition non-recursive.
        RefPtr<Object> Fallback; // a StyleValueBox (defined below)

        VariableReference() = default;
    };

    class StyleValue
    {
    public:
        enum class Kind
        {
            None,
            Color,
            Float,
            Thickness,
            Drawable,
            Bool,
            String,
            /// A length with units (percent, em, calc sums); plain numbers stay Float.
            Length,
            /// A BoxShadow tuple (box-shadow).
            Shadow,
            /// A StyleTransform (transform).
            Transform,
            /// A StylePoint (transform-origin).
            Point,
            /// An `animation:` list (AnimationList).
            Animations,
            /// A StyleFilter (filter, backdrop-filter).
            Filter,
            /// A `transition:` list (TransitionList).
            Transitions,
            /// The `inherit` keyword: take the parent's computed value.
            Inherit,
            /// The `initial` keyword: as if unset (the property's default).
            Initial,
            /// A `var(--name, fallback)` reference (see VariableReference).
            Variable
        };

        StyleValue() = default; // None

        [[nodiscard]] static StyleValue ColorVal(core::Color c)
        {
            StyleValue v;
            v.m_kind = Kind::Color;
            v.m_color = c;
            return v;
        }
        [[nodiscard]] static StyleValue FloatVal(f32 f)
        {
            StyleValue v;
            v.m_kind = Kind::Float;
            v.m_float = f;
            return v;
        }
        [[nodiscard]] static StyleValue ThicknessVal(Thickness t)
        {
            StyleValue v;
            v.m_kind = Kind::Thickness;
            v.m_thickness = t;
            return v;
        }
        [[nodiscard]] static StyleValue DrawableRef(RefPtr<Drawable> d)
        {
            StyleValue v;
            v.m_kind = Kind::Drawable;
            v.m_drawable = Move(d);
            return v;
        }
        [[nodiscard]] static StyleValue BoolVal(bool b)
        {
            StyleValue v;
            v.m_kind = Kind::Bool;
            v.m_bool = b;
            return v;
        }
        [[nodiscard]] static StyleValue StringRef(StringView s)
        {
            StyleValue v;
            v.m_kind = Kind::String;
            v.m_string = String(s);
            return v;
        }
        [[nodiscard]] static StyleValue LengthVal(Unit length)
        {
            StyleValue v;
            v.m_kind = Kind::Length;
            v.m_length = length;
            return v;
        }
        [[nodiscard]] static StyleValue AnimationsRef(RefPtr<AnimationList> list)
        {
            StyleValue v;
            v.m_kind = Kind::Animations;
            v.m_animations = Move(list);
            return v;
        }
        /// The animation list (null unless Kind::Animations); borrowed, valid while this value is.
        [[nodiscard]] const AnimationList* AsAnimations() const noexcept
        {
            return m_kind == Kind::Animations ? m_animations.Get() : nullptr;
        }

        [[nodiscard]] static StyleValue FilterVal(const StyleFilter& filter)
        {
            StyleValue v;
            v.m_kind = Kind::Filter;
            v.m_filter = filter;
            return v;
        }
        /// The filter a value gives: a Filter's, or a plain number as a blur's sigma (what code
        /// sets with SetStyle(Filter, sigma)).
        [[nodiscard]] Optional<StyleFilter> AsFilter() const
        {
            if (m_kind == Kind::Filter)
            {
                return m_filter;
            }
            if (m_kind == Kind::Float)
            {
                StyleFilter blurOnly;
                blurOnly.Blur = m_float;
                return blurOnly;
            }
            return {};
        }

        [[nodiscard]] static StyleValue PointVal(const StylePoint& point)
        {
            StyleValue v;
            v.m_kind = Kind::Point;
            v.m_point = point;
            return v;
        }
        [[nodiscard]] Optional<StylePoint> AsPoint() const
        {
            if (m_kind == Kind::Point)
            {
                return m_point;
            }
            return {};
        }

        [[nodiscard]] static StyleValue TransformVal(const StyleTransform& transform)
        {
            StyleValue v;
            v.m_kind = Kind::Transform;
            v.m_transform = transform;
            return v;
        }
        [[nodiscard]] Optional<StyleTransform> AsTransform() const
        {
            if (m_kind == Kind::Transform)
            {
                return m_transform;
            }
            return {};
        }

        /// The most shadows a value holds; a list written with more keeps its first ones.
        static constexpr usize kMaxShadows = 4;

        [[nodiscard]] static StyleValue ShadowVal(const BoxShadow& shadow)
        {
            return ShadowsVal(Span<const BoxShadow>(&shadow, 1));
        }
        /// A list of shadows, the first painted on top as CSS paints them.
        [[nodiscard]] static StyleValue ShadowsVal(Span<const BoxShadow> shadows)
        {
            StyleValue v;
            v.m_kind = Kind::Shadow;
            v.m_shadowCount = static_cast<u8>(Min(shadows.Size(), kMaxShadows));
            for (usize i = 0; i < v.m_shadowCount; ++i)
            {
                v.m_shadows[i] = shadows[i];
            }
            return v;
        }
        [[nodiscard]] static StyleValue TransitionsRef(RefPtr<TransitionList> list)
        {
            StyleValue v;
            v.m_kind = Kind::Transitions;
            v.m_transitions = Move(list);
            return v;
        }
        [[nodiscard]] static StyleValue Inherit()
        {
            StyleValue v;
            v.m_kind = Kind::Inherit;
            return v;
        }
        [[nodiscard]] static StyleValue Initial()
        {
            StyleValue v;
            v.m_kind = Kind::Initial;
            return v;
        }
        /// A reference to a custom property. `name` includes the leading `--`; the overload
        /// with `fallback` is used when the variable is unset.
        [[nodiscard]] static StyleValue VariableRef(IAllocator& allocator, StringView name);
        [[nodiscard]] static StyleValue VariableRef(IAllocator& allocator, StringView name,
                                                    const StyleValue& fallback);
        [[nodiscard]] static StyleValue None() { return StyleValue{}; }

        [[nodiscard]] Kind GetKind() const noexcept { return m_kind; }
        [[nodiscard]] bool IsNone() const noexcept { return m_kind == Kind::None; }
        /// Inherit, Initial or Variable: needs the cascade to turn it into a concrete value.
        [[nodiscard]] bool NeedsResolution() const noexcept
        {
            return m_kind == Kind::Inherit || m_kind == Kind::Initial || m_kind == Kind::Variable;
        }

        [[nodiscard]] Optional<Unit> AsLength() const
        {
            if (m_kind == Kind::Length)
            {
                return m_length;
            }
            return {};
        }
        /// The reference for a Variable value (null otherwise).
        [[nodiscard]] const VariableReference* Variable() const noexcept
        {
            return m_kind == Kind::Variable ? m_variable.Get() : nullptr;
        }

        /// The transition list (null unless Kind::Transitions). Borrowed: valid while this
        /// value (or the rule holding it) lives.
        [[nodiscard]] const TransitionList* AsTransitions() const noexcept
        {
            return m_kind == Kind::Transitions ? m_transitions.Get() : nullptr;
        }
        /// The first shadow (a text effect's only one).
        [[nodiscard]] Optional<BoxShadow> AsShadow() const
        {
            if (m_kind == Kind::Shadow && m_shadowCount > 0)
            {
                return m_shadows[0];
            }
            return {};
        }
        /// Every shadow, the first painted on top; empty for a value that is none.
        [[nodiscard]] Span<const BoxShadow> AsShadows() const noexcept
        {
            return m_kind == Kind::Shadow ? Span<const BoxShadow>(m_shadows, m_shadowCount) : Span<const BoxShadow>{};
        }
        [[nodiscard]] Optional<core::Color> AsColor() const
        {
            if (m_kind == Kind::Color)
            {
                return m_color;
            }
            return {};
        }
        [[nodiscard]] Optional<f32> AsFloat() const
        {
            if (m_kind == Kind::Float)
            {
                return m_float;
            }
            return {};
        }
        [[nodiscard]] Optional<Thickness> AsThickness() const
        {
            if (m_kind == Kind::Thickness)
            {
                return m_thickness;
            }
            return {};
        }
        [[nodiscard]] Optional<bool> AsBool() const
        {
            if (m_kind == Kind::Bool)
            {
                return m_bool;
            }
            return {};
        }

        /// Borrowed drawable pointer (owned by this value), or null if the kind differs.
        [[nodiscard]] Drawable* AsDrawable() const
        {
            return m_kind == Kind::Drawable ? m_drawable.Get() : nullptr;
        }
        /// Borrowed string view (backing owned by this value), or empty if the kind differs.
        [[nodiscard]] Optional<StringView> AsString() const
        {
            if (m_kind == Kind::String)
            {
                return m_string.AsView();
            }
            return {};
        }

    private:
        Kind m_kind = Kind::None;
        core::Color m_color{};
        f32 m_float = 0.0f;
        Thickness m_thickness{};
        RefPtr<Drawable> m_drawable;
        bool m_bool = false;
        String m_string;
        Unit m_length{};
        BoxShadow m_shadows[kMaxShadows]{};
        u8 m_shadowCount = 0;
        StyleTransform m_transform{};
        StylePoint m_point{};
        StyleFilter m_filter{};
        RefPtr<VariableReference> m_variable;
        RefPtr<TransitionList> m_transitions;
        RefPtr<AnimationList> m_animations;
    };

    /// Equality for transition retargeting: same kind and same payload (drawables by
    /// identity). Keywords and variables compare unequal (they are resolved before this).
    [[nodiscard]] inline bool StyleValueEquivalent(const StyleValue& a, const StyleValue& b)
    {
        if (a.GetKind() != b.GetKind())
        {
            return false;
        }
        switch (a.GetKind())
        {
        case StyleValue::Kind::None:
            return true;
        case StyleValue::Kind::Color:
            return a.AsColor().Value() == b.AsColor().Value();
        case StyleValue::Kind::Float:
            return a.AsFloat().Value() == b.AsFloat().Value();
        case StyleValue::Kind::Thickness:
            return a.AsThickness().Value() == b.AsThickness().Value();
        case StyleValue::Kind::Drawable:
            return a.AsDrawable() == b.AsDrawable();
        case StyleValue::Kind::Bool:
            return a.AsBool().Value() == b.AsBool().Value();
        case StyleValue::Kind::String:
            return a.AsString().Value() == b.AsString().Value();
        case StyleValue::Kind::Length:
            return a.AsLength().Value() == b.AsLength().Value();
        case StyleValue::Kind::Transform:
            return a.AsTransform().Value() == b.AsTransform().Value();
        case StyleValue::Kind::Point:
            return a.AsPoint().Value() == b.AsPoint().Value();
        case StyleValue::Kind::Filter:
            return a.AsFilter().Value() == b.AsFilter().Value();
        case StyleValue::Kind::Shadow:
        {
            const Span<const BoxShadow> sa = a.AsShadows();
            const Span<const BoxShadow> sb = b.AsShadows();
            if (sa.Size() != sb.Size())
            {
                return false;
            }
            for (usize i = 0; i < sa.Size(); ++i)
            {
                if (!(sa[i] == sb[i]))
                {
                    return false;
                }
            }
            return true;
        }
        case StyleValue::Kind::Transitions:
            return a.AsTransitions() == b.AsTransitions();
        case StyleValue::Kind::Animations:
            return a.AsAnimations() == b.AsAnimations();
        default:
            return false;
        }
    }

    /// Whether a transition can run from `a` to `b`: both numeric-like (Float and Length mix
    /// as lengths), both colors, thicknesses, shadows, or both drawables (cross-fade).
    [[nodiscard]] inline bool StyleValuesInterpolable(const StyleValue& a, const StyleValue& b)
    {
        const StyleValue::Kind ka = a.GetKind();
        const StyleValue::Kind kb = b.GetKind();
        const auto numeric = [](StyleValue::Kind k) {
            return k == StyleValue::Kind::Float || k == StyleValue::Kind::Length;
        };
        if (numeric(ka) && numeric(kb))
        {
            return true;
        }
        if (ka != kb)
        {
            return false;
        }
        return ka == StyleValue::Kind::Color || ka == StyleValue::Kind::Thickness ||
               ka == StyleValue::Kind::Shadow || ka == StyleValue::Kind::Transform || ka == StyleValue::Kind::Filter ||
               ka == StyleValue::Kind::Point ||
               ka == StyleValue::Kind::Drawable;
    }

    /// A length `t` of the way from `a` to `b`, component by component.
    [[nodiscard]] inline Unit LerpUnit(const Unit& a, const Unit& b, f32 t) noexcept
    {
        const auto lerp = [t](f32 x, f32 y) { return x + (y - x) * t; };
        Unit u = b;
        u.dp = lerp(a.dp, b.dp);
        u.pt = lerp(a.pt, b.pt);
        u.px = lerp(a.px, b.px);
        u.percent = lerp(a.percent, b.percent);
        u.em = lerp(a.em, b.em);
        return u;
    }

    /// A 2D matrix as translation, rotation, skew and scale: M = T * R(angle) * K(skew) * S, K the
    /// x-shear (x' = x + skew y), the scale's y taking a reflection's sign (the GUI's
    /// decomposition, after CSS's).
    struct DecomposedTransform
    {
        f32 Tx = 0.0f, Ty = 0.0f, Angle = 0.0f, Skew = 0.0f, Sx = 1.0f, Sy = 1.0f;

        [[nodiscard]] static DecomposedTransform Of(const Transform2D& m) noexcept
        {
            DecomposedTransform d;
            d.Tx = m.tx;
            d.Ty = m.ty;
            d.Sx = Sqrt(m.a * m.a + m.b * m.b);
            if (d.Sx <= 1e-12f)
            {
                d.Sx = 0.0f;
                d.Sy = Sqrt(m.c * m.c + m.d * m.d);
                return d;
            }
            d.Angle = Atan2(m.b, m.a);
            const f32 cs = Cos(d.Angle), sn = Sin(d.Angle);
            const f32 skewed = m.c * cs + m.d * sn;
            d.Sy = -m.c * sn + m.d * cs;
            d.Skew = d.Sy != 0.0f ? skewed / d.Sy : 0.0f;
            return d;
        }
        [[nodiscard]] Transform2D Matrix() const noexcept
        {
            Transform2D m;
            m.Translate(Tx, Ty);
            m.Rotate(Angle);
            m.Combine(Transform2D{1.0f, 0.0f, Skew, 1.0f, 0.0f, 0.0f});
            m.Scale(Sx, Sy);
            return m;
        }
    };

    /// A transform `t` of the way from `a` to `b`, as CSS interpolates them: function by function
    /// when both lists name the same functions (`none` padded with the other's identities, so
    /// `none` to rotate(90deg) turns from 0), else through their decomposed matrices, turning the
    /// short way. A translate's percentages need the view's size there, so lists that differ and
    /// hold one are resolved at a size of 0 (their percent parts drop out).
    [[nodiscard]] inline StyleTransform LerpTransforms(const StyleTransform& a, const StyleTransform& b, f32 t) noexcept
    {
        const auto lerp = [t](f32 x, f32 y) { return x + (y - x) * t; };
        StyleTransform from = a;
        StyleTransform to = b;
        if (from.Count == 0)
        {
            for (usize i = 0; i < to.Count; ++i)
                from.Push(to.Functions[i].Identity());
        }
        else if (to.Count == 0)
        {
            for (usize i = 0; i < from.Count; ++i)
                to.Push(from.Functions[i].Identity());
        }
        bool sameShape = from.Count == to.Count;
        for (usize i = 0; sameShape && i < from.Count; ++i)
            sameShape = from.Functions[i].Kind == to.Functions[i].Kind;
        StyleTransform out;
        if (sameShape)
        {
            for (usize i = 0; i < from.Count; ++i)
            {
                const TransformFunction& fa = from.Functions[i];
                const TransformFunction& fb = to.Functions[i];
                TransformFunction f = fa;
                f.X = LerpUnit(fa.X, fb.X, t);
                f.Y = LerpUnit(fa.Y, fb.Y, t);
                f.A = lerp(fa.A, fb.A);
                f.B = lerp(fa.B, fb.B);
                for (usize k = 0; k < 6; ++k)
                    f.M[k] = lerp(fa.M[k], fb.M[k]);
                out.Push(f);
            }
            return out;
        }
        const DecomposedTransform x = DecomposedTransform::Of(from.Compose(core::Float2{0.0f, 0.0f}, 1.0f));
        const DecomposedTransform y = DecomposedTransform::Of(to.Compose(core::Float2{0.0f, 0.0f}, 1.0f));
        f32 turn = y.Angle - x.Angle; // the short way round
        while (turn > kPi)
            turn -= kTwoPi;
        while (turn < -kPi)
            turn += kTwoPi;
        DecomposedTransform mixed;
        mixed.Tx = lerp(x.Tx, y.Tx);
        mixed.Ty = lerp(x.Ty, y.Ty);
        mixed.Angle = x.Angle + turn * t;
        mixed.Skew = lerp(x.Skew, y.Skew);
        mixed.Sx = lerp(x.Sx, y.Sx);
        mixed.Sy = lerp(x.Sy, y.Sy);
        const Transform2D m = mixed.Matrix();
        TransformFunction f;
        f.Kind = TransformFunctionKind::Matrix;
        const f32 values[6] = {m.a, m.b, m.c, m.d, m.tx, m.ty};
        for (usize k = 0; k < 6; ++k)
            f.M[k] = values[k];
        out.Push(f);
        return out;
    }

    /// The value `t` of the way from `a` to `b` (t clamped to 0..1). Colors, floats,
    /// thicknesses, lengths (component-wise; a Float promotes to a dp Length when mixed) and
    /// shadows interpolate; every other kind switches at the midpoint (CSS discrete
    /// animation), drawables included, whose cross-fade the draw path handles separately.
    [[nodiscard]] inline StyleValue LerpStyleValue(const StyleValue& a, const StyleValue& b, f32 t)
    {
        if (t <= 0.0f)
        {
            return a;
        }
        if (t >= 1.0f)
        {
            return b;
        }
        const auto lerp = [t](f32 x, f32 y) { return x + (y - x) * t; };
        const StyleValue::Kind ka = a.GetKind();
        const StyleValue::Kind kb = b.GetKind();
        if (ka == StyleValue::Kind::Float && kb == StyleValue::Kind::Float)
        {
            return StyleValue::FloatVal(lerp(a.AsFloat().Value(), b.AsFloat().Value()));
        }
        const auto numeric = [](StyleValue::Kind k) {
            return k == StyleValue::Kind::Float || k == StyleValue::Kind::Length;
        };
        if (numeric(ka) && numeric(kb))
        {
            const Unit ua = ka == StyleValue::Kind::Length ? a.AsLength().Value() : Unit::Dp(a.AsFloat().Value());
            const Unit ub = kb == StyleValue::Kind::Length ? b.AsLength().Value() : Unit::Dp(b.AsFloat().Value());
            Unit u = ub;
            u.dp = lerp(ua.dp, ub.dp);
            u.pt = lerp(ua.pt, ub.pt);
            u.px = lerp(ua.px, ub.px);
            u.percent = lerp(ua.percent, ub.percent);
            u.em = lerp(ua.em, ub.em);
            return StyleValue::LengthVal(u);
        }
        if (ka != kb)
        {
            return t < 0.5f ? a : b;
        }
        switch (ka)
        {
        case StyleValue::Kind::Color:
        {
            const core::Color ca = a.AsColor().Value();
            const core::Color cb = b.AsColor().Value();
            return StyleValue::ColorVal(core::Color{lerp(ca.r, cb.r), lerp(ca.g, cb.g), lerp(ca.b, cb.b), lerp(ca.a, cb.a)});
        }
        case StyleValue::Kind::Thickness:
        {
            const Thickness ta = a.AsThickness().Value();
            const Thickness tb = b.AsThickness().Value();
            return StyleValue::ThicknessVal(Thickness{lerp(ta.Left, tb.Left), lerp(ta.Top, tb.Top),
                                                      lerp(ta.Right, tb.Right), lerp(ta.Bottom, tb.Bottom)});
        }
        case StyleValue::Kind::Filter:
        {
            const StyleFilter fa = a.AsFilter().Value();
            const StyleFilter fb = b.AsFilter().Value();
            StyleFilter out;
            out.Blur = lerp(fa.Blur, fb.Blur);
            out.Saturation = lerp(fa.Saturation, fb.Saturation);
            out.Brightness = lerp(fa.Brightness, fb.Brightness);
            return StyleValue::FilterVal(out);
        }
        case StyleValue::Kind::Transform:
            return StyleValue::TransformVal(LerpTransforms(a.AsTransform().Value(), b.AsTransform().Value(), t));
        case StyleValue::Kind::Point:
        {
            const StylePoint pa = a.AsPoint().Value();
            const StylePoint pb = b.AsPoint().Value();
            return StyleValue::PointVal(StylePoint{LerpUnit(pa.X, pb.X, t), LerpUnit(pa.Y, pb.Y, t)});
        }
        case StyleValue::Kind::Shadow:
        {
            // Shadow by shadow; the shorter list is padded with transparent shadows of the other's
            // kind, as CSS pads it, so a shadow added on hover fades in.
            const Span<const BoxShadow> la = a.AsShadows();
            const Span<const BoxShadow> lb = b.AsShadows();
            const usize count = Max(la.Size(), lb.Size());
            BoxShadow out[StyleValue::kMaxShadows];
            for (usize i = 0; i < count; ++i)
            {
                const auto clear = [](const BoxShadow& like)
                {
                    BoxShadow none;
                    none.Color = core::Color{like.Color.r, like.Color.g, like.Color.b, 0.0f};
                    none.Inset = like.Inset;
                    return none;
                };
                const BoxShadow sa = i < la.Size() ? la[i] : clear(lb[i]);
                const BoxShadow sb = i < lb.Size() ? lb[i] : clear(la[i]);
                BoxShadow& s = out[i];
                s.OffsetX = lerp(sa.OffsetX, sb.OffsetX);
                s.OffsetY = lerp(sa.OffsetY, sb.OffsetY);
                s.Blur = lerp(sa.Blur, sb.Blur);
                s.Spread = lerp(sa.Spread, sb.Spread);
                s.Color = core::Color{lerp(sa.Color.r, sb.Color.r), lerp(sa.Color.g, sb.Color.g),
                                      lerp(sa.Color.b, sb.Color.b), lerp(sa.Color.a, sb.Color.a)};
                s.Inset = t < 0.5f ? sa.Inset : sb.Inset;
            }
            return StyleValue::ShadowsVal(Span<const BoxShadow>(out, count));
        }
        default:
            return t < 0.5f ? a : b;
        }
    }

    /// Boxes a StyleValue as an Object so a VariableReference can own its fallback.
    class StyleValueBox : public Object
    {
        RTTI_OBJECT(StyleValueBox, Object)
    public:
        StyleValue Value;
        StyleValueBox() = default;
        explicit StyleValueBox(StyleValue value) : Value(Move(value)) {}
    };

    inline StyleValue StyleValue::VariableRef(IAllocator& allocator, StringView name)
    {
        return VariableRef(allocator, name, StyleValue{});
    }

    inline StyleValue StyleValue::VariableRef(IAllocator& allocator, StringView name,
                                              const StyleValue& fallback)
    {
        RefPtr<VariableReference> ref = MakeRef<VariableReference>(allocator);
        ref->Name = String(name);
        ref->NameHash = HashText(name);
        if (!fallback.IsNone())
        {
            ref->Fallback = MakeRef<StyleValueBox>(allocator, fallback);
        }
        StyleValue v;
        v.m_kind = Kind::Variable;
        v.m_variable = Move(ref);
        return v;
    }

    /// The fallback of a reference as a StyleValue (None when there is none).
    [[nodiscard]] inline StyleValue VariableFallback(const VariableReference& ref)
    {
        if (const StyleValueBox* box = Cast<StyleValueBox>(ref.Fallback.Get()))
        {
            return box->Value;
        }
        return StyleValue::None();
    }

    RTTI_DEFINE_OBJECT(VariableReference, "rtti::ui")
    RTTI_DEFINE_OBJECT(StyleValueBox, "rtti::ui")
    RTTI_DEFINE_OBJECT(TransitionList, "rtti::ui")
    RTTI_DEFINE_OBJECT(AnimationList, "rtti::ui")
}
