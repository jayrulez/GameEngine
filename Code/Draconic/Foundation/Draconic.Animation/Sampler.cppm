/// Draconic::Animation - the `:sampler` partition.
///
/// Stateless sampling of clips/tracks into bone poses + pose blending (lerp + additive). Ported
/// faithfully from Sedulous.Animation.AnimationSampler.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.animation:sampler;

import draconic.foundation;
import :skeleton; // BoneTransform, Skeleton
import :clip;     // AnimationTrack, AnimationClip, InterpolationMode

using namespace draconic::foundation;

export namespace draconic::animation
{

    // --- cubic spline (Hermite) helpers ---
    [[nodiscard]] inline Float3 CubicSplineVec3(const Keyframe<Float3>& prev,
                                                const Keyframe<Float3>& next, f32 t, f32 duration)
    {
        const f32 t2 = t * t, t3 = t2 * t;
        const Float3 p0 = prev.value;
        const Float3 m0 = prev.outTangent * duration;
        const Float3 p1 = next.value;
        const Float3 m1 = next.inTangent * duration;
        const f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const f32 h10 = t3 - 2.0f * t2 + t;
        const f32 h01 = -2.0f * t3 + 3.0f * t2;
        const f32 h11 = t3 - t2;
        return p0 * h00 + m0 * h10 + p1 * h01 + m1 * h11;
    }

    [[nodiscard]] inline Quaternion CubicSplineQuat(const Keyframe<Quaternion>& prev,
                                                    const Keyframe<Quaternion>& next, f32 t,
                                                    f32 /*duration*/)
    {
        // Simplified Hermite + normalize (squad would be more accurate), matching Sedulous.
        const f32 t2 = t * t, t3 = t2 * t;
        const f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const f32 h01 = -2.0f * t3 + 3.0f * t2;
        return Normalized(Slerp(prev.value, next.value, h01 / (h00 + h01)));
    }

    // Sample a Float3 track at `time` (returns `defaultValue` if empty).
    [[nodiscard]] inline Float3 SampleVec3(const AnimationTrack<Float3>* track, f32 time,
                                           Float3 defaultValue = Float3{0, 0, 0})
    {
        if (track == nullptr || track->Keyframes().IsEmpty())
        {
            return defaultValue;
        }
        const KeyframeLookup k = track->FindKeyframes(time);
        if (k.prev < 0)
        {
            return defaultValue;
        }
        const Keyframe<Float3>& prev = track->Keyframes()[static_cast<usize>(k.prev)];
        const Keyframe<Float3>& next = track->Keyframes()[static_cast<usize>(k.next)];
        switch (track->interpolation)
        {
        case InterpolationMode::Step:
            return prev.value;
        case InterpolationMode::Linear:
            return Lerp(prev.value, next.value, k.t);
        case InterpolationMode::CubicSpline:
            return CubicSplineVec3(prev, next, k.t, next.time - prev.time);
        }
        return prev.value;
    }

    // Sample a Quaternion track at `time` (returns `defaultValue` if empty).
    [[nodiscard]] inline Quaternion SampleQuat(const AnimationTrack<Quaternion>* track, f32 time,
                                               Quaternion defaultValue = Quaternion::Identity)
    {
        if (track == nullptr || track->Keyframes().IsEmpty())
        {
            return defaultValue;
        }
        const KeyframeLookup k = track->FindKeyframes(time);
        if (k.prev < 0)
        {
            return defaultValue;
        }
        const Keyframe<Quaternion>& prev = track->Keyframes()[static_cast<usize>(k.prev)];
        const Keyframe<Quaternion>& next = track->Keyframes()[static_cast<usize>(k.next)];
        switch (track->interpolation)
        {
        case InterpolationMode::Step:
            return prev.value;
        case InterpolationMode::Linear:
            return Slerp(prev.value, next.value, k.t);
        case InterpolationMode::CubicSpline:
            return CubicSplineQuat(prev, next, k.t, next.time - prev.time);
        }
        return prev.value;
    }

    // Sample a clip at `time` into `outPoses` (bind pose for un-animated bones). Handles looping/clamp.
    inline void SampleClip(const AnimationClip& clip, const Skeleton& skeleton, f32 time,
                           Span<BoneTransform> outPoses)
    {
        const i32 boneCount = skeleton.BoneCount();
        for (i32 i = 0; i < boneCount && static_cast<usize>(i) < outPoses.Size(); ++i)
        {
            const Bone* bone = skeleton.GetBone(i);
            outPoses[static_cast<usize>(i)] =
                (bone != nullptr) ? bone->localBindPose : BoneTransform{};
        }

        f32 sampleTime = time;
        if (clip.isLooping && clip.duration > 0.0f)
        {
            sampleTime = time - clip.duration * Floor(time / clip.duration); // positive modulo
            if (sampleTime < 0.0f)
            {
                sampleTime += clip.duration;
            }
        }
        else
        {
            sampleTime = Clamp(time, 0.0f, clip.duration);
        }

        for (const auto& track : clip.PositionTracks())
        {
            const i32 b = track->boneIndex;
            if (b >= 0 && static_cast<usize>(b) < outPoses.Size())
            {
                outPoses[static_cast<usize>(b)].position =
                    SampleVec3(track.Get(), sampleTime, outPoses[static_cast<usize>(b)].position);
            }
        }
        for (const auto& track : clip.RotationTracks())
        {
            const i32 b = track->boneIndex;
            if (b >= 0 && static_cast<usize>(b) < outPoses.Size())
            {
                outPoses[static_cast<usize>(b)].rotation =
                    SampleQuat(track.Get(), sampleTime, outPoses[static_cast<usize>(b)].rotation);
            }
        }
        for (const auto& track : clip.ScaleTracks())
        {
            const i32 b = track->boneIndex;
            if (b >= 0 && static_cast<usize>(b) < outPoses.Size())
            {
                outPoses[static_cast<usize>(b)].scale =
                    SampleVec3(track.Get(), sampleTime, outPoses[static_cast<usize>(b)].scale);
            }
        }
    }

    // Linear blend of two poses (0 = a, 1 = b).
    inline void BlendPoses(Span<const BoneTransform> a, Span<const BoneTransform> b, f32 factor,
                           Span<BoneTransform> out)
    {
        const usize count = Min(Min(a.Size(), b.Size()), out.Size());
        for (usize i = 0; i < count; ++i)
        {
            out[i] = BoneTransform::Lerp(a[i], b[i], factor);
        }
    }

    // Additive blend: base + (additive * weight).
    inline void AdditivePoses(Span<const BoneTransform> base, Span<const BoneTransform> additive,
                              f32 weight, Span<BoneTransform> out)
    {
        const usize count = Min(Min(base.Size(), additive.Size()), out.Size());
        for (usize i = 0; i < count; ++i)
        {
            out[i].position = base[i].position + additive[i].position * weight;
            out[i].rotation =
                Slerp(Quaternion::Identity, additive[i].rotation, weight) * base[i].rotation;
            out[i].scale = base[i].scale * Lerp(Float3::One, additive[i].scale, weight);
        }
    }

} // namespace draconic::animation
