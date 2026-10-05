// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Animation - the `:rootmotion` partition.
///
/// The travel a clip's baked root motion curve carries between two of its times (root-motion.md
/// P1): a translation and a turn about +Y. A delta is expressed in the frame the character faces
/// at its start, so applying deltas one after another (Compose) turns each by the turns before
/// it, the way a character that already turned walks on in its new direction. A step that wraps
/// a looping clip is split at the end, and a step spanning whole loops adds the full loop's delta
/// once per loop (nothing is lost however far one step goes).

module;
#include "Core/Prelude.h"

export module foundation.animation:rootmotion;

import foundation.core;
import :clip;

using namespace foundation::core;

export namespace foundation::animation
{
    /// A step of root motion: how far the character moves (in the frame it faces at the step's
    /// start; model space when the clip does not extract its turn) and how far it turns.
    struct RootMotionDelta
    {
        Float3 translation = Float3::Zero;
        f32 yaw = 0.0f; // radians about +Y

        [[nodiscard]] bool IsZero() const noexcept { return LengthSquared(translation) == 0.0f && yaw == 0.0f; }
    };

    /// `a`, then `b`: b's translation was measured after a's turn.
    [[nodiscard]] inline RootMotionDelta Compose(const RootMotionDelta& a, const RootMotionDelta& b) noexcept
    {
        return RootMotionDelta{a.translation + RotateVector(Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, a.yaw),
                                                            b.translation),
                               a.yaw + b.yaw};
    }

    /// Deltas blend as poses do: `t` of the way from `a` to `b` (a crossfade, a blend tree).
    [[nodiscard]] inline RootMotionDelta BlendRootMotion(const RootMotionDelta& a, const RootMotionDelta& b, f32 t) noexcept
    {
        return RootMotionDelta{a.translation + (b.translation - a.translation) * t, a.yaw + (b.yaw - a.yaw) * t};
    }

    namespace root_motion_detail
    {
        struct CurvePoint
        {
            Float3 position;
            f32 yaw;
        };

        /// The curve at `time` (clamped to its ends), linear between its samples.
        [[nodiscard]] inline CurvePoint At(const RootMotionCurve& curve, f32 time) noexcept
        {
            const usize n = Min(curve.times.Size(), Min(curve.positions.Size(), curve.yaws.Size()));
            if (n == 0)
            {
                return CurvePoint{Float3::Zero, 0.0f};
            }
            if (time <= curve.times[0])
            {
                return CurvePoint{curve.positions[0], curve.yaws[0]};
            }
            if (time >= curve.times[n - 1])
            {
                return CurvePoint{curve.positions[n - 1], curve.yaws[n - 1]};
            }
            usize lo = 0;
            usize hi = n - 1;
            while (hi - lo > 1)
            {
                const usize mid = (lo + hi) / 2;
                (curve.times[mid] <= time ? lo : hi) = mid;
            }
            const f32 span = curve.times[hi] - curve.times[lo];
            const f32 t = span > 0.0f ? (time - curve.times[lo]) / span : 0.0f;
            return CurvePoint{curve.positions[lo] + (curve.positions[hi] - curve.positions[lo]) * t,
                              curve.yaws[lo] + (curve.yaws[hi] - curve.yaws[lo]) * t};
        }

        /// From `from` to `to`, both within the clip: the extracted parts only, in the facing frame
        /// at `from` (a clip that does not extract its turn moves in model space).
        [[nodiscard]] inline RootMotionDelta Between(const AnimationClip& clip, f32 from, f32 to) noexcept
        {
            const RootMotionCurve& curve = clip.rootMotion;
            const CurvePoint a = At(curve, from);
            const CurvePoint b = At(curve, to);
            Float3 travel = b.position - a.position;
            if (!curve.horizontal)
            {
                travel.x = 0.0f;
                travel.z = 0.0f;
            }
            if (!curve.vertical)
            {
                travel.y = 0.0f;
            }
            RootMotionDelta delta;
            if (curve.yaw)
            {
                // The character already turned by (a.yaw - the clip's first yaw): measure in its frame.
                const f32 turned = a.yaw - (curve.yaws.IsEmpty() ? 0.0f : curve.yaws[0]);
                delta.translation = RotateVector(Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, -turned), travel);
                delta.yaw = b.yaw - a.yaw;
            }
            else
            {
                delta.translation = travel;
            }
            return delta;
        }
    }

    /// The root motion `clip` carries from `from` to `to`, seconds, UNWRAPPED: for a looping clip
    /// a time past the end is a later loop (0.9 to 2.1 in a one-second clip crosses the end twice),
    /// and a backward step (to < from) runs it in reverse. A clip that does not loop clamps both
    /// times to its length. Zero for a clip with no root motion.
    [[nodiscard]] inline RootMotionDelta ClipRootMotion(const AnimationClip& clip, f32 from, f32 to, bool looping)
    {
        const RootMotionCurve& curve = clip.rootMotion;
        const f32 duration = clip.duration;
        if (curve.IsEmpty() || duration <= 0.0f || from == to)
        {
            return RootMotionDelta{};
        }
        if (!looping)
        {
            return root_motion_detail::Between(clip, Clamp(from, 0.0f, duration), Clamp(to, 0.0f, duration));
        }
        const f32 loopFrom = Floor(from / duration);
        const f32 loopTo = Floor(to / duration);
        const f32 inFrom = from - loopFrom * duration;
        const f32 inTo = to - loopTo * duration;
        if (loopFrom == loopTo)
        {
            return root_motion_detail::Between(clip, inFrom, inTo);
        }
        const bool forward = to > from;
        const f32 end = forward ? duration : 0.0f;
        const f32 start = forward ? 0.0f : duration;
        RootMotionDelta delta = root_motion_detail::Between(clip, inFrom, end);
        const RootMotionDelta loop = root_motion_detail::Between(clip, start, end);
        const i32 whole = static_cast<i32>(Abs(loopTo - loopFrom)) - 1;
        for (i32 i = 0; i < whole; ++i)
        {
            delta = Compose(delta, loop);
        }
        return Compose(delta, root_motion_detail::Between(clip, start, inTo));
    }
}
