// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::PropertyAnimation - module `foundation.propertyanimation`.
//
// The data model, evaluation and reflection binding resolver for animating any reflected property
// on any component with keyframe curves, as data. It depends only on foundation.core reflection
// and is kept apart from foundation.animation (skeletal), so headless consumers (the editor page,
// tests, MCP hosts) evaluate a property clip without pulling in bones, poses and skinning.
//
// A clip is a set of tracks; each track targets one component type and property path and holds
// either per-component scalar Curves (Float, Float3, Color) or quaternion keys (Quat, slerped,
// never per-component Euler). Evaluation yields a Variant per track at time t; the binding
// resolver walks the reflected property chain (nested paths included) and writes through
// reflection set, so resolve-mode rules and any setter side effects apply exactly as if the
// inspector wrote the value. Nothing here knows the scene: the resolver takes a reflection
// Instance, and the engine binds the entity to its component.

module;
#include "Core/Prelude.h"

export module foundation.propertyanimation;

import foundation.core;

using namespace foundation::core;

export namespace foundation::propertyanimation
{
    // The value a track animates. Float, Float3 and Color are per-component scalar curves; Quat is
    // slerped keys. The wire stores the kind as a byte, so step tracks (Bool, enums) can be added.
    enum class TrackValueKind : u8
    {
        Float,
        Float3,
        Quat,
        Color,
    };

    // Scalar channels a kind uses (Float=1, Float3=3, Color=4). Quat=0 (it uses quatKeys, not curves).
    [[nodiscard]] inline u32 ChannelCount(TrackValueKind kind) noexcept
    {
        switch (kind)
        {
        case TrackValueKind::Float:
            return 1;
        case TrackValueKind::Float3:
            return 3;
        case TrackValueKind::Color:
            return 4;
        case TrackValueKind::Quat:
            return 0;
        }
        return 0;
    }

    // A quaternion keyframe (Quat tracks). Slerp between neighbours; shortest-arc handled by Slerp.
    struct QuatKey
    {
        f32 time = 0.0f;
        Quaternion value = Quaternion::Identity;
    };

    // Max channel count a scalar track needs (Color = 4).
    inline constexpr u32 kMaxChannels = 4;

    // One animated property. `componentType` is the reflected component's name (the engine resolves
    // it to a live component on the owning entity); `propertyPath` is dot-joined for nested props
    // (e.g. "color" or "light.color"). Channel storage: Float uses channels[0]; Float3 channels[0..2];
    // Color channels[0..3] (r,g,b,a); Quat uses quatKeys.
    struct PropertyTrack
    {
        String componentType;
        String propertyPath;
        TrackValueKind kind = TrackValueKind::Float;
        Curve channels[kMaxChannels];
        Array<QuatKey> quatKeys;

        // The track's own time span: the longest of its active channels / quat keys.
        [[nodiscard]] f32 Duration() const noexcept
        {
            f32 d = 0.0f;
            const u32 channelCount = ChannelCount(kind);
            for (u32 i = 0; i < channelCount; ++i)
            {
                d = Max(d, channels[i].Duration());
            }
            if (!quatKeys.IsEmpty())
            {
                d = Max(d, quatKeys[quatKeys.Size() - 1].time);
            }
            return d;
        }

        // Sample the track at `time` into a Variant of the matching type (Float->f32, Float3->Float3,
        // Color->Color, Quat->Quaternion). Callers wrap/clamp `time` (loop/pingpong is the clip's job).
        [[nodiscard]] Variant Sample(f32 time) const
        {
            switch (kind)
            {
            case TrackValueKind::Float:
                return Variant::From(channels[0].Evaluate(time));
            case TrackValueKind::Float3:
                return Variant::From(Float3{channels[0].Evaluate(time), channels[1].Evaluate(time),
                                            channels[2].Evaluate(time)});
            case TrackValueKind::Color:
                return Variant::From(Color{channels[0].Evaluate(time), channels[1].Evaluate(time),
                                           channels[2].Evaluate(time), channels[3].Evaluate(time)});
            case TrackValueKind::Quat:
                return Variant::From(SampleQuat(time));
            }
            return Variant{};
        }

        // Sample, but keep `current`'s value for any channel that has no keys. An empty channel does
        // not drive its component; otherwise it would sample 0 and, say, teleport an entity that
        // only animates X to y = z = 0. `current` is the target property's live value; if it is empty
        // (unreadable) an empty channel falls back to 0. A track with all channels empty returns
        // `current` unchanged (a no-op write). Quat: empty quatKeys keeps `current`.
        [[nodiscard]] Variant SampleMerged(f32 time, const Variant& current) const
        {
            switch (kind)
            {
            case TrackValueKind::Float:
                return channels[0].KeyCount() > 0 ? Variant::From(channels[0].Evaluate(time)) : current;
            case TrackValueKind::Float3:
            {
                const Float3 base = current.Is<Float3>() ? current.Get<Float3>() : Float3{};
                return Variant::From(Float3{
                    channels[0].KeyCount() > 0 ? channels[0].Evaluate(time) : base.x,
                    channels[1].KeyCount() > 0 ? channels[1].Evaluate(time) : base.y,
                    channels[2].KeyCount() > 0 ? channels[2].Evaluate(time) : base.z});
            }
            case TrackValueKind::Color:
            {
                const Color base = current.Is<Color>() ? current.Get<Color>() : Color{};
                return Variant::From(Color{
                    channels[0].KeyCount() > 0 ? channels[0].Evaluate(time) : base.r,
                    channels[1].KeyCount() > 0 ? channels[1].Evaluate(time) : base.g,
                    channels[2].KeyCount() > 0 ? channels[2].Evaluate(time) : base.b,
                    channels[3].KeyCount() > 0 ? channels[3].Evaluate(time) : base.a});
            }
            case TrackValueKind::Quat:
                return quatKeys.IsEmpty() ? current : Variant::From(SampleQuat(time));
            }
            return current;
        }

        // Slerp the quaternion keys at `time` (clamped to the ends; shortest-arc via Slerp).
        [[nodiscard]] Quaternion SampleQuat(f32 time) const noexcept
        {
            const usize count = quatKeys.Size();
            if (count == 0)
            {
                return Quaternion::Identity;
            }
            if (count == 1 || time <= quatKeys[0].time)
            {
                return quatKeys[0].value;
            }
            if (time >= quatKeys[count - 1].time)
            {
                return quatKeys[count - 1].value;
            }
            usize i = 0;
            while (i + 1 < count && quatKeys[i + 1].time <= time)
            {
                ++i;
            }
            const QuatKey& a = quatKeys[i];
            const QuatKey& b = quatKeys[i + 1];
            const f32 segment = b.time - a.time;
            if (segment <= 1e-6f)
            {
                return b.value;
            }
            return Slerp(a.value, b.value, (time - a.time) / segment);
        }
    };

    // A cooked or authored clip: a duration and the tracks. Immutable shared data at runtime (each
    // playing component owns only its time, state and binding cache). Loop behavior is the animator
    // component's loopMode, not a clip field.
    struct PropertyAnimationClip
    {
        f32 duration = 0.0f;
        Array<PropertyTrack> tracks;

        // The clip's duration, its longest track's (call after editing to refresh `duration`).
        [[nodiscard]] f32 ComputeDuration() const noexcept
        {
            f32 d = 0.0f;
            for (const PropertyTrack& t : tracks)
            {
                d = Max(d, t.Duration());
            }
            return d;
        }
    };

    // ===================================================================================
    // Binding resolver (reflection only, no scene dependency)
    // ===================================================================================

    // A resolved property path: the chain of PropertyInfo from the component type down to the leaf.
    // PropertyInfo pointers are stable static type metadata, so caching the chain is safe; the nested
    // sub-instance addresses are re-walked on every write, because a raw component or sub-object
    // pointer does not survive a structural change. An empty chain means unresolved.
    struct PropertyBinding
    {
        Array<const PropertyInfo*> chain;

        [[nodiscard]] bool IsResolved() const noexcept { return !chain.IsEmpty(); }
    };

    // Resolve a dot-joined `propertyPath` against `componentType`, walking Nested properties for the
    // intermediate segments. Returns an unresolved binding (empty chain) if any segment is missing or
    // an intermediate segment is not a Nested struct. Reads only static type metadata, no instance.
    [[nodiscard]] PropertyBinding ResolveBinding(const TypeInfo& componentType, StringView propertyPath);

    // Write `value` through a resolved binding into `componentInstance`. Re-derives each nested
    // sub-instance address from the live instance (never cached), then reflection-sets the leaf.
    // Returns InvalidArgument for an unresolved binding, NotFound if a nested address is null, or the
    // leaf setter's status. The value's type must match the leaf property (the caller's Sample does).
    [[nodiscard]] Status WriteBinding(const PropertyBinding& binding, const Instance& componentInstance,
                                      const Variant& value);

    // Read the current leaf value through a resolved binding (the symmetric inverse of WriteBinding).
    // Re-derives each nested sub-instance address from the live instance, then reflection-gets the
    // leaf. Returns an empty Variant for an unresolved binding, an empty instance, or a null nested
    // address. Used to snapshot a property before a transient write (the in-scene animation preview).
    [[nodiscard]] Variant ReadBinding(const PropertyBinding& binding, const Instance& componentInstance);
}
