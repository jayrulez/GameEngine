// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :tween partition
//
// Motion as an API (E14 of Specs/gui-lab-evolution-2.md): a tween moves one property of a node
// toward a value on its GUI's clock, eased by a CSS easing or a named curve. A property a sheet
// names composes over the cascade, as CSS's animation origin does: the tween writes its progress
// into the node's animated layer and the style pass interpolates there, through the typed values
// the sheets' transitions use, from the start given or the value underneath. Any other name is a
// reflected property, read and written through its accessors (numbers, colours and points move;
// what cannot switches halfway). A tween may wait, repeat and run back and forth; at its end it
// holds its value until stopped or replaced. It holds its node weakly (the action's weak target).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:tween;

import foundation.core;
import :action;
import :node;
import :ui_node;
import :css_values;
import :style_value;
import :style_properties;
import :style_sheet;   // ResolvedStyle::ParseLonghand
import :style_applier; // the reflected properties' lookup and parsing, as a sheet's
import :easing;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// A mark no other tween has had (in the library, so every binary draws from one count).
    [[nodiscard]] u64 NextTweenOwner() noexcept;

    class Tween : public Action
    {
        RTTI_OBJECT(Tween, Action)
    public:
        Tween(core::String property, core::String to, f32 seconds, Easing easing)
            : m_property(core::Move(property)), m_toText(core::Move(to)), m_seconds(core::Max(0.0f, seconds)), m_easing(easing)
        {
            m_owner = NextTweenOwner();
        }

        // === Shaping (before its first frame) ===
        /// Where it starts, as a value of its property (else the value showing).
        Tween& From(core::StringView text)
        {
            m_fromText = core::String(text);
            m_hasFrom = true;
            return *this;
        }
        /// How long it waits first, in seconds.
        Tween& Delay(f32 seconds)
        {
            m_delay = core::Max(0.0f, seconds);
            return *this;
        }
        /// How many times it runs (1 by default; a negative count runs until stopped).
        Tween& Repeat(i32 count)
        {
            m_iterations = count;
            return *this;
        }
        /// Every other run goes back from the end to the start.
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
        /// Called once it reaches its end (not when stopped or replaced).
        Tween& OnFinished(core::Function<void()> callback)
        {
            m_onFinished = core::Move(callback);
            return *this;
        }

        [[nodiscard]] core::StringView GetProperty() const noexcept { return m_property.AsView(); }
        /// Whether it composes over the cascade (a property a sheet names) rather than through a
        /// reflected property. Known once it has run a frame.
        [[nodiscard]] bool MovesStyle() const noexcept { return m_route == Route::Style; }

        /// Stops it and gives its property back to the sheet (a reflected property keeps the
        /// value it has).
        void Cancel()
        {
            if (m_route == Route::Style)
                if (UINode* node = core::Cast<UINode>(ResolveTarget()))
                    if (const MotionValue* motion = node->FindMotion(m_property.AsView()); motion != nullptr && motion->Owner == m_owner)
                        (void)node->RemoveMotion(m_property.AsView());
            m_done = true;
        }
        /// Another tween took its property: it stops where it is, leaving the value to the new one.
        void Replace() noexcept { m_done = true; }

        // === Action ===
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
            Node* target = ResolveTarget();
            if (target == nullptr)
            {
                m_done = true;
                return;
            }
            m_elapsed += elapsed.AsSecondsF();
            const f32 active = m_elapsed - m_delay;
            if (active < 0.0f)
                return; // waiting: the property shows what it did
            if (m_route == Route::Unbound && !Bind(*target))
            {
                m_done = true;
                return;
            }
            // Superseded through the layer (another writer took the property): stop.
            if (m_route == Route::Style)
            {
                const MotionValue* motion = core::Cast<UINode>(target)->FindMotion(m_property.AsView());
                if (motion == nullptr || motion->Owner != m_owner)
                {
                    m_done = true;
                    return;
                }
            }
            bool finished = false;
            f32 local = 1.0f;
            u32 run = 0;
            if (m_seconds <= 0.0f)
            {
                finished = true;
                run = m_iterations > 0 ? static_cast<u32>(m_iterations - 1) : 0;
            }
            else
            {
                const f32 runs = active / m_seconds;
                if (m_iterations >= 0 && runs >= static_cast<f32>(core::Max(m_iterations, 1)))
                {
                    finished = true;
                    run = static_cast<u32>(core::Max(m_iterations, 1) - 1);
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
            Write(*target, m_easing.Evaluate(local));
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

    private:
        enum class Route : u8
        {
            Unbound,
            Style,
            Reflected,
        };

        // The property a sheet does not name, by its CSS name or its reflected one.
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

        // Its first frame: which way it writes, and its values parsed.
        bool Bind(Node& target)
        {
            if (FindStyleProperty(m_property.AsView()) != nullptr)
            {
                UINode* node = core::Cast<UINode>(&target);
                if (node == nullptr)
                    return false;
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
                    motion.From = before->Shown; // from where the tween it replaces left it
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

        // The value at eased progress `t`.
        void Write(Node& target, f32 t)
        {
            if (m_route == Route::Style)
            {
                UINode& node = *core::Cast<UINode>(&target);
                if (MotionValue* motion = node.FindMotion(m_property.AsView()))
                {
                    motion->Progress = t;
                    node.MarkStyleMotionDirty();
                }
                return;
            }
            (void)core::SetProperty(*m_reflected, Instance::From(&target), Mix(m_from, m_to, t));
        }

        // Numbers, colours and points move; anything else switches halfway, as CSS's discrete values.
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

        Route m_route = Route::Unbound;
        const PropertyInfo* m_reflected = nullptr;
        Variant m_from;
        Variant m_to;
        u64 m_owner = 0; // its mark in the layer
        f32 m_elapsed = 0.0f;
        f32 m_progress = 0.0f;
        bool m_done = false;
    };

    RTTI_DEFINE_OBJECT(Tween, "rtti::gui")
}
