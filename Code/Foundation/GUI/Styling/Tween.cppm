// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :tween partition
//
// Motion as an API. A tween moves one property of a node toward a value on its GUI's clock,
// eased by a CSS easing or a named curve. When a sheet can name the property, the tween composes
// over the cascade, as the CSS animation origin does: it writes its progress into the node's
// animated layer, and the style pass interpolates there with the typed values the sheets'
// transitions use, starting from the given start or from the value underneath. Any other name is
// a reflected property, read and written through its accessors. Numbers, colours and points move
// smoothly; other values switch halfway. A tween can wait, repeat and run back and forth, and
// when it ends it holds its value until it is stopped or replaced. It holds its node weakly, as
// an action holds its target. A tween of a GUI data value (GUIContext::AnimateData) has no node:
// it moves a number in the data model, and whatever is bound to that value follows (a score
// counting up). The value keeps its type, so a whole number counts in whole numbers.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:tween;

import foundation.core;
import foundation.script; // IScriptDelegate, for a script callback when the tween ends
import :action;
import :node;
import :ui_node;
import :css_values;
import :style_value;
import :style_properties;
import :style_sheet;   // ResolvedStyle::ParseLonghand
import :style_applier; // looks up and parses reflected properties as a sheet does
import :style_interpolation;
import :easing;
import :data_model; // tweens of data values

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// Returns an owner mark no other tween has had. It is defined in the library so every binary
    /// draws from one counter.
    [[nodiscard]] u64 NextTweenOwner() noexcept;

    class Tween;
    /// Makes any other tween of the same property on `tween`'s node, running alone or in a group,
    /// give way to `tween`. Defined in TweenImpl.cpp, which can see the action manager.
    void ReplaceOthersOf(Tween& tween);

    class Tween : public Action
    {
        RTTI_OBJECT(Tween, Action)
    public:
        Tween(core::String property, core::String to, f32 seconds, Easing easing)
            : m_property(core::Move(property)), m_toText(core::Move(to)), m_seconds(core::Max(0.0f, seconds)), m_easing(easing)
        {
            m_owner = NextTweenOwner();
        }

        /// Moves the data value its property names in `model`, rather than a node's property.
        void TargetData(RefPtr<DataModel> model) { m_data = core::Move(model); }
        [[nodiscard]] bool MovesData() const noexcept { return m_data.Get() != nullptr; }

        // === Shaping, before the first frame ===
        /// Sets the start, written as a value of the property. Without it the tween starts from
        /// the value showing.
        Tween& From(core::StringView text)
        {
            m_fromText = core::String(text);
            m_hasFrom = true;
            return *this;
        }
        /// Sets how long the tween waits before it starts, in seconds.
        Tween& Delay(f32 seconds)
        {
            m_delay = core::Max(0.0f, seconds);
            return *this;
        }
        /// Sets how many times the tween runs: 1 by default, and a negative count runs until
        /// stopped.
        Tween& Repeat(i32 count)
        {
            m_iterations = count;
            return *this;
        }
        /// Makes every second run go back from the end to the start.
        Tween& PingPong(bool on = true)
        {
            m_pingPong = on;
            return *this;
        }
        Tween& Ease(const Easing& easing)
        {
            m_easing = easing;
            return *this;
        }
        /// Makes a data value's tween count in whole numbers, rounding each value it writes to the
        /// nearest (for a score). Scripts need this because their numbers all arrive as fractions,
        /// so the value's type cannot tell.
        Tween& Whole(bool on = true)
        {
            m_whole = on;
            return *this;
        }
        /// Moves the value as a damped spring toward its end instead of over a fixed time, like the
        /// springs of GPUI and react-spring. `stiffness` pulls toward the end, `damping` slows the
        /// motion and `mass` resists it; with little damping it overshoots and rings. Duration,
        /// easing, repeats and ping-pong do not apply, and the tween ends once the spring settles.
        Tween& Spring(f32 stiffness = 170.0f, f32 damping = 26.0f, f32 mass = 1.0f)
        {
            m_spring = true;
            m_stiffness = core::Max(0.0f, stiffness);
            m_damping = core::Max(0.0f, damping);
            m_mass = core::Max(1e-4f, mass);
            return *this;
        }
        [[nodiscard]] bool IsSpring() const noexcept { return m_spring; }
        /// Sends the tween toward a new end from where it is now and keeps its speed, so a drag
        /// that settles or a menu that follows the pointer moves without a jolt.
        void Retarget(core::StringView to);
        /// Sets a callback for when the tween reaches its end. It is not called when the tween is
        /// stopped or replaced.
        Tween& OnFinished(core::Function<void()> callback)
        {
            m_onFinished = core::Move(callback);
            return *this;
        }

        // === The script surface, reflected in GUIReflectionImpl.cpp. Each call returns the tween,
        // so a script can chain calls as native code does.
        RefPtr<Tween> FromScript(core::StringView text) { return Self(From(text)); }
        RefPtr<Tween> DelayScript(f32 seconds) { return Self(Delay(seconds)); }
        RefPtr<Tween> RepeatScript(i32 count) { return Self(Repeat(count)); }
        RefPtr<Tween> PingPongScript(bool on) { return Self(PingPong(on)); }
        RefPtr<Tween> EaseScript(core::StringView easing) { return Self(Ease(ParseEasing(easing).ValueOr(Easing::Ease()))); }
        RefPtr<Tween> WholeScript(bool on) { return Self(Whole(on)); }
        RefPtr<Tween> SpringScript(f32 stiffness, f32 damping, f32 mass) { return Self(Spring(stiffness, damping, mass)); }
        RefPtr<Tween> OnFinishedScript(RefPtr<foundation::script::IScriptDelegate> handler)
        {
            if (handler)
                OnFinished([handler]() { (void)handler->Invoke(Span<core::Variant>{}); });
            return RefPtr<Tween>(this);
        }
        [[nodiscard]] bool IsFinished() { return IsDone(); }

        [[nodiscard]] core::StringView GetProperty() const noexcept { return m_property.AsView(); }
        /// Whether the tween composes over the cascade (a property a sheet names) rather than
        /// writing a reflected property. This is known only once it has run a frame.
        [[nodiscard]] bool MovesStyle() const noexcept { return m_route == Route::Style; }

        /// Stops the tween and gives its property back to the sheet. A reflected property keeps
        /// the value it has.
        void Cancel()
        {
            if (m_data && m_data->TweenOwnerOf(m_property.AsView()) == m_owner)
                m_data->ClaimTween(m_property.AsView(), 0); // the value stays as it is
            if (m_route == Route::Style)
                if (UINode* node = core::Cast<UINode>(ResolveTarget()))
                    if (const MotionValue* motion = node->FindMotion(m_property.AsView()); motion != nullptr && motion->Owner == m_owner)
                        (void)node->RemoveMotion(m_property.AsView());
            m_done = true;
        }
        /// Called when another tween takes over the property. This tween stops where it is and
        /// leaves the value to the new one.
        void Replace() noexcept { m_done = true; }

        // === Action ===
        /// Starts the tween when it is run, when its group reaches it, or again for the group's next
        /// run. It takes its property from any other tween moving it, and reads its values afresh
        /// on its first frame.
        void Start() override
        {
            m_elapsed = 0.0f;
            m_overrun = 0.0f;
            m_done = false;
            m_route = Route::Unbound;
            m_springX = 0.0f;
            m_springV = 0.0f;
            if (m_data)
                m_data->ClaimTween(m_property.AsView(), m_owner); // the tween moving it now gives way
            else
                ReplaceOthersOf(*this);
        }
        void Stop() override { m_done = true; }
        void Update(core::Duration elapsed) override
        {
            if (m_done)
                return;
            Node* target = ResolveTarget();
            if (target == nullptr && !m_data)
            {
                m_done = true;
                return;
            }
            m_elapsed += elapsed.AsSecondsF();
            const f32 active = m_elapsed - m_delay;
            if (active < 0.0f)
                return; // still waiting, so the property keeps showing its value
            if (m_route == Route::Unbound && !(m_data ? BindData() : Bind(*target)))
            {
                m_done = true;
                return;
            }
            // Another tween took over the data value, so stop where it is.
            if (m_route == Route::Data && m_data->TweenOwnerOf(m_property.AsView()) != m_owner)
            {
                m_done = true;
                return;
            }
            // Another writer took the property in the animated layer, so stop.
            if (m_route == Route::Style)
            {
                const MotionValue* motion = core::Cast<UINode>(target)->FindMotion(m_property.AsView());
                if (motion == nullptr || motion->Owner != m_owner)
                {
                    m_done = true;
                    return;
                }
            }
            if (m_spring)
            {
                StepSpring(core::Min(active, elapsed.AsSecondsF()));
                const bool settled = Abs(1.0f - m_springX) < 1e-3f && Abs(m_springV) < 1e-3f;
                if (settled)
                    m_springX = 1.0f;
                m_progress = core::Clamp(m_springX, 0.0f, 1.0f);
                Write(target, m_springX);
                if (settled)
                {
                    m_done = true;
                    if (m_onFinished)
                        m_onFinished();
                    FireDone();
                }
                return;
            }
            bool finished = false;
            f32 local = 1.0f;
            u32 run = 0;
            if (m_seconds <= 0.0f)
            {
                finished = true;
                run = m_iterations > 0 ? static_cast<u32>(m_iterations - 1) : 0;
                m_overrun = active;
            }
            else
            {
                const f32 runs = active / m_seconds;
                if (m_iterations >= 0 && runs >= static_cast<f32>(core::Max(m_iterations, 1)))
                {
                    finished = true;
                    run = static_cast<u32>(core::Max(m_iterations, 1) - 1);
                    m_overrun = active - m_seconds * static_cast<f32>(core::Max(m_iterations, 1));
                }
                else
                {
                    run = static_cast<u32>(Floor(runs));
                    local = runs - static_cast<f32>(run);
                }
            }
            if (m_pingPong && (run % 2u) == 1u)
                local = 1.0f - local;
            m_progress = local;
            Write(target, m_easing.Evaluate(local));
            if (finished)
            {
                m_done = true;
                if (m_onFinished)
                    m_onFinished();
                FireDone();
            }
        }
        [[nodiscard]] bool IsDone() override { return m_done; }
        [[nodiscard]] f32 GetCurrentProgress() override { return m_progress; }
        [[nodiscard]] core::Duration GetTotalTime() override
        {
            return core::Duration::FromSeconds(static_cast<f64>(m_delay + m_seconds * static_cast<f32>(core::Max(m_iterations, 1))));
        }
        [[nodiscard]] bool CanMoveTarget() const override { return !IsPaintOnlyStyleProperty(m_property.AsView()); }
        [[nodiscard]] core::Duration GetOverrun() const override { return core::Duration::FromSeconds(static_cast<f64>(m_overrun)); }

    private:
        enum class Route : u8
        {
            Unbound,
            Style,
            Reflected,
            Data, // a data model's value
        };

        [[nodiscard]] RefPtr<Tween> Self(Tween& self) { return RefPtr<Tween>(&self); }

        // Advances the spring's progress toward 1 by `seconds`, in small steps. Semi-implicit
        // Euler keeps a stiff spring steady where one big step would fly off.
        void StepSpring(f32 seconds)
        {
            constexpr f32 kStep = 1.0f / 240.0f;
            while (seconds > 0.0f)
            {
                const f32 h = core::Min(seconds, kStep);
                const f32 force = -m_stiffness * (m_springX - 1.0f) - m_damping * m_springV;
                m_springV += force / m_mass * h;
                m_springX += m_springV * h;
                seconds -= h;
            }
        }
        // How far apart two values are, where that has a measure (a number, a length of one unit,
        // a colour, a point); 0 where it has none.
        [[nodiscard]] static f32 Distance(const StyleValue& a, const StyleValue& b)
        {
            if (a.Kind != b.Kind)
                return 0.0f;
            switch (a.Kind)
            {
            case StyleValueKind::Number:
                return Abs(b.Number - a.Number);
            case StyleValueKind::Length:
                return a.Length.Unit == b.Length.Unit ? Abs(b.Length.Value - a.Length.Value) : 0.0f;
            case StyleValueKind::ColorValue:
            {
                const Color d{b.ColorValue.r - a.ColorValue.r, b.ColorValue.g - a.ColorValue.g, b.ColorValue.b - a.ColorValue.b,
                              b.ColorValue.a - a.ColorValue.a};
                return Sqrt(d.r * d.r + d.g * d.g + d.b * d.b + d.a * d.a);
            }
            default:
                return 0.0f;
            }
        }
        [[nodiscard]] static f32 Distance(const Variant& a, const Variant& b)
        {
            if (const f32* x = a.TryGet<f32>())
                if (const f32* y = b.TryGet<f32>())
                    return Abs(*y - *x);
            if (const f64* x = a.TryGet<f64>())
                if (const f64* y = b.TryGet<f64>())
                    return static_cast<f32>(*y > *x ? *y - *x : *x - *y);
            if (const i32* x = a.TryGet<i32>())
                if (const i32* y = b.TryGet<i32>())
                    return Abs(static_cast<f32>(*y - *x));
            if (const core::Float2* x = a.TryGet<core::Float2>())
                if (const core::Float2* y = b.TryGet<core::Float2>())
                    return core::Length(core::Float2{y->x - x->x, y->y - x->y});
            return 0.0f;
        }
        // Carries the spring's speed over to a new span. Where both spans have a measure, the
        // speed stays the same in the value's own units; otherwise it stays the same in progress.
        void CarrySpeed(f32 before, f32 after) noexcept
        {
            if (before > 0.0f && after > 0.0f)
                m_springV *= before / after;
            m_springX = 0.0f;
        }

        // Finds a property that sheets do not name, by its CSS name or its reflected name.
        [[nodiscard]] static const PropertyInfo* FindReflected(const Node& node, core::StringView name)
        {
            for (const TypeInfo* type = node.GetType(); type != nullptr; type = type->base)
                for (usize i = 0; i < PropertyCount(*type); ++i)
                {
                    const PropertyInfo& property = PropertyAt(*type, i);
                    const Attribute* css = FindAttribute(property, u8"css");
                    const core::String* cssName = css != nullptr ? css->value.TryGet<core::String>() : nullptr;
                    if ((cssName != nullptr && cssName->AsView() == name) ||
                        core::StringView(reinterpret_cast<const char8_t*>(property.name)) == name)
                        return &property;
                }
            return nullptr;
        }

        // On the first frame, decides how the tween writes its property and parses its values.
        bool Bind(Node& target)
        {
            // On a node that sheets style, a property a sheet names goes through the style. On any
            // other node the tween writes the reflected property (a plain node's opacity, x, y).
            if (UINode* node = core::Cast<UINode>(&target); node != nullptr && FindStyleProperty(m_property.AsView()) != nullptr)
            {
                MotionValue motion;
                motion.Property = m_property;
                motion.To = ResolvedStyle::ParseLonghand(m_property.AsView(), m_toText.AsView());
                if (motion.To.Kind == StyleValueKind::None)
                    return false;
                if (m_hasFrom)
                {
                    motion.From = ResolvedStyle::ParseLonghand(m_property.AsView(), m_fromText.AsView());
                    motion.HasFrom = motion.From.Kind != StyleValueKind::None;
                }
                else if (const MotionValue* before = node->FindMotion(m_property.AsView()); before != nullptr && before->HasShown)
                {
                    // Start from where the previous tween is now. Its progress may have moved since
                    // the style pass last showed it, if it ended this frame before the pass ran.
                    motion.From = before->HasUnder ? InterpolateStyleValue(before->Under, before->To, before->Progress) : before->Shown;
                    motion.HasFrom = true;
                }
                motion.Owner = m_owner;
                (void)node->SetMotion(core::Move(motion));
                m_route = Route::Style;
                return true;
            }
            m_reflected = FindReflected(target, m_property.AsView());
            if (m_reflected == nullptr || m_reflected->set == nullptr)
                return false;
            const Instance instance = Instance::From(&target);
            m_to = detail::ParseForProperty(*m_reflected, m_toText.AsView(), LengthContext{});
            if (m_to.IsEmpty())
                return false;
            m_from = m_hasFrom ? detail::ParseForProperty(*m_reflected, m_fromText.AsView(), LengthContext{})
                               : core::GetProperty(*m_reflected, instance);
            if (m_from.IsEmpty())
                m_from = core::GetProperty(*m_reflected, instance);
            m_route = Route::Reflected;
            return true;
        }

        // The first frame for a data value: move from the current value (or the given start)
        // toward the end, both as numbers. The value keeps its type, or becomes a number when it
        // has none yet.
        bool BindData()
        {
            const Optional<f64> to = ParseFloat(core::Trim(m_toText.AsView()));
            if (!to.HasValue())
                return false;
            const Variant now = m_data->Peek(m_property.AsView());
            f64 from = 0.0;
            (void)detail::NumberOf(now, from);
            if (m_hasFrom)
                from = ParseFloat(core::Trim(m_fromText.AsView())).ValueOr(from);
            f64 probe = 0.0;
            m_dataType = !now.IsEmpty() && detail::NumberOf(now, probe) && now.TryGet<bool>() == nullptr ? now.Type() : &TypeOf<f64>();
            m_from = Variant::From<f64>(from);
            m_to = Variant::From<f64>(to.Value());
            m_route = Route::Data;
            return true;
        }

        // Writes the value at eased progress `t`.
        void Write(Node* target, f32 t)
        {
            if (m_route == Route::Data)
            {
                Variant value = Mix(m_from, m_to, t);
                if (m_whole)
                    if (const f64* n = value.TryGet<f64>())
                        value = Variant::From<f64>(*n < 0.0 ? -Floor(static_cast<f32>(-*n + 0.5)) : Floor(static_cast<f32>(*n + 0.5)));
                const Variant typed = m_dataType != nullptr ? ConvertVariant(value, *m_dataType) : value;
                (void)m_data->Set(m_property.AsView(), typed.IsEmpty() ? value : typed);
                return;
            }
            if (target == nullptr)
                return;
            if (m_route == Route::Style)
            {
                UINode& node = *core::Cast<UINode>(target);
                if (MotionValue* motion = node.FindMotion(m_property.AsView()))
                {
                    motion->Progress = t;
                    node.MarkStyleMotionDirty();
                }
                return;
            }
            (void)core::SetProperty(*m_reflected, Instance::From(target), Mix(m_from, m_to, t));
        }

        // Numbers, colours and points move; anything else switches halfway, as discrete values do
        // in CSS.
        [[nodiscard]] static Variant Mix(const Variant& a, const Variant& b, f32 t)
        {
            const auto lerp = [t](f32 x, f32 y) { return x + (y - x) * t; };
            if (const f32* x = a.TryGet<f32>())
                if (const f32* y = b.TryGet<f32>())
                    return Variant::From<f32>(lerp(*x, *y));
            if (const f64* x = a.TryGet<f64>())
                if (const f64* y = b.TryGet<f64>())
                    return Variant::From<f64>(*x + (*y - *x) * static_cast<f64>(t));
            if (const i32* x = a.TryGet<i32>())
                if (const i32* y = b.TryGet<i32>())
                    return Variant::From<i32>(static_cast<i32>(Round(lerp(static_cast<f32>(*x), static_cast<f32>(*y)))));
            if (const Color* x = a.TryGet<Color>())
                if (const Color* y = b.TryGet<Color>())
                    return Variant::From<Color>(Color{lerp(x->r, y->r), lerp(x->g, y->g), lerp(x->b, y->b), lerp(x->a, y->a)});
            if (const core::Float2* x = a.TryGet<core::Float2>())
                if (const core::Float2* y = b.TryGet<core::Float2>())
                    return Variant::From<core::Float2>(core::Float2{lerp(x->x, y->x), lerp(x->y, y->y)});
            return t < 0.5f ? a : b;
        }

        core::String m_property;
        core::String m_toText;
        core::String m_fromText;
        bool m_hasFrom = false;
        f32 m_seconds = 0.0f;
        f32 m_delay = 0.0f;
        i32 m_iterations = 1;
        bool m_pingPong = false;
        Easing m_easing;
        core::Function<void()> m_onFinished;

        bool m_spring = false;
        f32 m_stiffness = 170.0f;
        f32 m_damping = 26.0f;
        f32 m_mass = 1.0f;
        f32 m_springX = 0.0f; // progress, past 1 when it overshoots
        f32 m_springV = 0.0f; // the spring's speed, in progress per second

        Route m_route = Route::Unbound;
        const PropertyInfo* m_reflected = nullptr;
        RefPtr<DataModel> m_data;               // the model whose value it moves, or none
        const TypeInfo* m_dataType = nullptr;   // the value's type, kept while it moves
        bool m_whole = false;                   // for a data value: whole numbers only
        Variant m_from;
        Variant m_to;
        u64 m_owner = 0; // this tween's owner mark in the animated layer
        f32 m_elapsed = 0.0f;
        f32 m_overrun = 0.0f;
        f32 m_progress = 0.0f;
        bool m_done = false;
    };

    // StaticType comes from REFLECT_MEMBERS(Tween) in Scene/GUIReflectionImpl.cpp, which
    // reflects the script surface.

    inline void Tween::Retarget(core::StringView to)
    {
        m_toText = core::String(to);
        Node* target = ResolveTarget();
        if ((target == nullptr && !m_data) || m_route == Route::Unbound)
            return; // it reads its end at its first frame
        if (!m_spring)
            m_elapsed = m_delay; // a timed tween runs its full time again, from where it is
        if (m_route == Route::Style)
        {
            UINode* node = core::Cast<UINode>(target);
            MotionValue* motion = node != nullptr ? node->FindMotion(m_property.AsView()) : nullptr;
            const StyleValue next = ResolvedStyle::ParseLonghand(m_property.AsView(), to);
            if (motion == nullptr || motion->Owner != m_owner || next.Kind == StyleValueKind::None)
                return;
            const StyleValue now = motion->HasUnder ? InterpolateStyleValue(motion->Under, motion->To, motion->Progress) : motion->Shown;
            const f32 before = motion->HasUnder ? Distance(motion->Under, motion->To) : 0.0f;
            CarrySpeed(before, Distance(now, next));
            motion->From = now;
            motion->HasFrom = true;
            motion->To = next;
            motion->Progress = 0.0f;
            node->MarkStyleMotionDirty();
            return;
        }
        if (m_route == Route::Data)
        {
            const Optional<f64> end = ParseFloat(core::Trim(to));
            if (!end.HasValue())
                return;
            // Start from the value showing, which is where the tween is apart from a whole
            // number's rounding.
            const Variant now = Mix(m_from, m_to, m_spring ? m_springX : m_easing.Evaluate(m_progress));
            const Variant next = Variant::From<f64>(end.Value());
            CarrySpeed(Distance(m_from, m_to), Distance(now, next));
            m_from = now;
            m_to = next;
            return;
        }
        const Variant next = detail::ParseForProperty(*m_reflected, to, LengthContext{});
        if (next.IsEmpty())
            return;
        const Variant now = Mix(m_from, m_to, m_springX);
        CarrySpeed(Distance(m_from, m_to), Distance(now, next));
        m_from = now;
        m_to = next;
    }

    // === Groups ===

    /// A step of a group that waits.
    class TweenWait : public Action
    {
        RTTI_OBJECT(TweenWait, Action)
    public:
        explicit TweenWait(f32 seconds) noexcept : m_seconds(core::Max(0.0f, seconds)) {}
        void Start() override
        {
            m_elapsed = 0.0f;
            m_done = false;
        }
        void Stop() override { m_done = true; }
        void Update(core::Duration elapsed) override
        {
            if (m_done)
                return;
            m_elapsed += elapsed.AsSecondsF();
            m_done = m_elapsed >= m_seconds;
        }
        [[nodiscard]] bool IsDone() override { return m_done; }
        [[nodiscard]] f32 GetCurrentProgress() override { return m_seconds > 0.0f ? core::Min(1.0f, m_elapsed / m_seconds) : 1.0f; }
        [[nodiscard]] core::Duration GetTotalTime() override { return core::Duration::FromSeconds(static_cast<f64>(m_seconds)); }
        [[nodiscard]] bool CanMoveTarget() const override { return false; }
        [[nodiscard]] core::Duration GetOverrun() const override
        {
            return core::Duration::FromSeconds(static_cast<f64>(core::Max(0.0f, m_elapsed - m_seconds)));
        }

    private:
        f32 m_seconds;
        f32 m_elapsed = 0.0f;
        bool m_done = false;
    };
    RTTI_DEFINE_OBJECT(TweenWait, "rtti::gui")

    /// A step of a group that calls back, taking no time.
    class TweenCall : public Action
    {
        RTTI_OBJECT(TweenCall, Action)
    public:
        explicit TweenCall(core::Function<void()> callback) : m_callback(core::Move(callback)) {}
        void Start() override
        {
            m_done = false;
            m_overrun = 0.0f;
        }
        void Stop() override { m_done = true; }
        void Update(core::Duration elapsed) override
        {
            if (m_done)
                return;
            m_done = true;
            m_overrun = elapsed.AsSecondsF();
            if (m_callback)
                m_callback();
        }
        [[nodiscard]] bool IsDone() override { return m_done; }
        [[nodiscard]] f32 GetCurrentProgress() override { return m_done ? 1.0f : 0.0f; }
        [[nodiscard]] core::Duration GetTotalTime() override { return core::Duration{}; }
        [[nodiscard]] bool CanMoveTarget() const override { return false; }
        [[nodiscard]] core::Duration GetOverrun() const override { return core::Duration::FromSeconds(static_cast<f64>(m_overrun)); }

    private:
        core::Function<void()> m_callback;
        f32 m_overrun = 0.0f;
        bool m_done = false;
    };
    RTTI_DEFINE_OBJECT(TweenCall, "rtti::gui")

    /// Steps that run as one: tweens of any nodes, waits, calls and other groups, run one after
    /// another (TweenSequence) or together (TweenParallel). The group repeats as a whole and calls
    /// back at the end. Run it on a node with RunAction; the node's GUI keeps its time, and each
    /// step keeps its own target.
    class TweenGroup : public Action
    {
        RTTI_OBJECT(TweenGroup, Action)
    public:
        TweenGroup& Add(RefPtr<Action> step)
        {
            if (step)
                m_steps.PushBack(core::Move(step));
            return *this;
        }
        TweenGroup& Wait(f32 seconds) { return Add(core::MakeRef<TweenWait>(MemoryAllocator(), seconds)); }
        TweenGroup& Call(core::Function<void()> callback)
        {
            return Add(core::MakeRef<TweenCall>(MemoryAllocator(), core::Move(callback)));
        }
        /// Sets how many times the group runs: 1 by default, and a negative count runs until
        /// stopped.
        TweenGroup& Repeat(i32 count)
        {
            m_iterations = count;
            return *this;
        }
        TweenGroup& OnFinished(core::Function<void()> callback)
        {
            m_onFinished = core::Move(callback);
            return *this;
        }
        [[nodiscard]] usize StepCount() const noexcept { return m_steps.Size(); }

        // === The script surface, reflected in GUIReflectionImpl.cpp. Each call returns the group,
        // so a script can chain calls as Add and the rest do.
        RefPtr<TweenGroup> AddScript(RefPtr<Tween> tween) { return Self(Add(core::Move(tween))); }
        RefPtr<TweenGroup> AddGroupScript(RefPtr<TweenGroup> group) { return Self(Add(core::Move(group))); }
        RefPtr<TweenGroup> WaitScript(f32 seconds) { return Self(Wait(seconds)); }
        RefPtr<TweenGroup> CallScript(RefPtr<foundation::script::IScriptDelegate> handler)
        {
            return Self(Call([handler]() { if (handler) (void)handler->Invoke(Span<core::Variant>{}); }));
        }
        RefPtr<TweenGroup> RepeatScript(i32 count) { return Self(Repeat(count)); }
        RefPtr<TweenGroup> OnFinishedScript(RefPtr<foundation::script::IScriptDelegate> handler)
        {
            if (handler)
                OnFinished([handler]() { (void)handler->Invoke(Span<core::Variant>{}); });
            return RefPtr<TweenGroup>(this);
        }
        [[nodiscard]] bool IsFinished() { return IsDone(); }

        /// Calls `f(step)` for each step running now, so a replacement can find a tween inside a
        /// group.
        template <typename F>
        void ForEachRunning(F&& f)
        {
            for (const RefPtr<Action>& step : m_steps)
                if (IsRunning(*step))
                    f(*step);
        }

        void Start() override
        {
            m_run = 0;
            m_done = false;
            m_overrun = 0.0f;
            StartRun();
        }
        void Stop() override
        {
            for (const RefPtr<Action>& step : m_steps)
                step->Stop();
            m_done = true;
        }
        void Update(core::Duration elapsed) override
        {
            f32 seconds = elapsed.AsSecondsF();
            for (u32 guard = 0; !m_done && guard < 64; ++guard)
            {
                f32 over = 0.0f;
                if (!Advance(seconds, over))
                    return;
                ++m_run;
                if (m_iterations >= 0 && m_run >= static_cast<u32>(core::Max(m_iterations, 1)))
                {
                    m_done = true;
                    m_overrun = over;
                    if (m_onFinished)
                        m_onFinished();
                    FireDone();
                    return;
                }
                StartRun();
                if (over <= 0.0f)
                    return;
                seconds = over; // the next run gets the time this one left over
            }
        }
        [[nodiscard]] bool IsDone() override { return m_done; }
        [[nodiscard]] f32 GetCurrentProgress() override { return 0.0f; }
        [[nodiscard]] core::Duration GetTotalTime() override { return core::Duration{}; }
        [[nodiscard]] bool CanMoveTarget() const override
        {
            for (const RefPtr<Action>& step : m_steps)
                if (step->CanMoveTarget())
                    return true;
            return false;
        }
        [[nodiscard]] core::Duration GetOverrun() const override { return core::Duration::FromSeconds(static_cast<f64>(m_overrun)); }

    protected:
        [[nodiscard]] RefPtr<TweenGroup> Self(TweenGroup& self) { return RefPtr<TweenGroup>(&self); }
        virtual void StartRun() = 0;
        /// Advances the steps by `seconds`. Returns true when this run has ended, with the time
        /// left over in `overrun`.
        virtual bool Advance(f32 seconds, f32& overrun) = 0;
        [[nodiscard]] virtual bool IsRunning(const Action& step) const = 0;

        Array<RefPtr<Action>> m_steps;

    private:
        i32 m_iterations = 1;
        u32 m_run = 0;
        f32 m_overrun = 0.0f;
        bool m_done = false;
        core::Function<void()> m_onFinished;
    };
    // StaticType comes from REFLECT_MEMBERS(TweenGroup) in Scene/GUIReflectionImpl.cpp, which
    // reflects the script surface.

    /// Runs steps one after another. Each starts when the previous one ends, with the time it
    /// left over.
    class TweenSequence : public TweenGroup
    {
        RTTI_OBJECT(TweenSequence, TweenGroup)
    protected:
        void StartRun() override
        {
            m_index = 0;
            if (!m_steps.IsEmpty())
                m_steps[0]->Start();
        }
        bool Advance(f32 seconds, f32& overrun) override
        {
            while (m_index < m_steps.Size())
            {
                Action& step = *m_steps[m_index];
                step.Update(core::Duration::FromSeconds(static_cast<f64>(seconds)));
                if (!step.IsDone())
                    return false;
                seconds = step.GetOverrun().AsSecondsF();
                if (++m_index < m_steps.Size())
                    m_steps[m_index]->Start();
            }
            overrun = seconds;
            return true;
        }
        [[nodiscard]] bool IsRunning(const Action& step) const override
        {
            return m_index < m_steps.Size() && m_steps[m_index].Get() == &step;
        }

    private:
        usize m_index = 0;
    };
    // StaticType comes from REFLECT_MEMBERS(TweenSequence) in Scene/GUIReflectionImpl.cpp, which
    // reflects the script surface.

    /// Runs steps together. The run ends when the last of them ends.
    class TweenParallel : public TweenGroup
    {
        RTTI_OBJECT(TweenParallel, TweenGroup)
    protected:
        void StartRun() override
        {
            for (const RefPtr<Action>& step : m_steps)
                step->Start();
        }
        bool Advance(f32 seconds, f32& overrun) override
        {
            bool all = true;
            f32 least = seconds;
            for (const RefPtr<Action>& step : m_steps)
            {
                if (step->IsDone())
                    continue;
                step->Update(core::Duration::FromSeconds(static_cast<f64>(seconds)));
                if (step->IsDone())
                    least = core::Min(least, step->GetOverrun().AsSecondsF());
                else
                    all = false;
            }
            overrun = least; // the time past the end of the step that ended last
            return all;
        }
        [[nodiscard]] bool IsRunning(const Action& step) const override { return !const_cast<Action&>(step).IsDone(); }
    };
    // StaticType comes from REFLECT_MEMBERS(TweenParallel) in Scene/GUIReflectionImpl.cpp, which
    // reflects the script surface.
}
