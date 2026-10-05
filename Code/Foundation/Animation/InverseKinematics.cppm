// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Animation - the `:ik` partition.
///
/// Inverse kinematics solvers (inverse-kinematics.md P1): pure functions over a skeleton, a LOCAL
/// pose and its model-space matrices (ModelPoseCache), with no physics and no scene. A solve writes
/// local rotations and keeps the cache current by rebuilding from the highest bone it changed down,
/// so bones above the chain never move. Targets are in the skeleton's model space; the caller owns
/// time (fades) and order. Nothing here allocates: the only buffer is the cache's, sized once.
///
/// Rotations compose as the engine's row-vector math does: a bone's model rotation is
/// `parentModel * local` (Hamilton order, the right factor applied first), so a model-space turn D
/// of a bone is `inverse(parentModel) * D * parentModel * local` in its local rotation.

module;
#include "Core/Prelude.h"

export module foundation.animation:ik;

import foundation.core;
import :skeleton;
import :modifier;

using namespace foundation::core;

export namespace foundation::animation
{
    /// What a solve did: whether the chain's end reached its target, and how far it missed (a
    /// distance for a two-bone chain, an angle in radians for an aim). `valid` is false when the
    /// bones do not form the chain (a bad index, or a bone not below the one before it); nothing
    /// changed then.
    struct IkResult
    {
        bool valid = false;
        bool reached = false;
        f32 error = 0.0f;
    };

    /// A two-bone chain (a thigh, a shin, a foot; an upper arm, a forearm, a hand). Each bone must be
    /// below the one before it; twist bones between them are carried along.
    struct TwoBoneIkChain
    {
        i32 start = -1;
        i32 mid = -1;
        i32 end = -1;
    };

    struct TwoBoneIkSettings
    {
        Float3 target = Float3::Zero; // model space
        bool matchRotation = false;   // turn the end bone to targetRotation as well
        Quaternion targetRotation = Quaternion::Identity; // model space
        // The bend plane, first that applies: a pole (the mid joint bends toward it), the
        // animated chain's own bend, the bind pose's bend, then hingeAxis. A knee and an elbow
        // bend opposite ways, so the hinge has no default: it is the mid joint's rotation axis in
        // the START bone's local space (the mid moves toward cross(hinge, chain direction)).
        bool hasPole = false;
        Float3 pole = Float3::Zero; // model space
        Float3 hingeAxis = Float3::Zero;
        f32 weight = 1.0f; // 0 leaves the pose as it was, byte for byte
    };

    /// An aim (a head, a spine sharing a turn, a weapon): the last bone's aim axis points at the
    /// target, each bone taking its share of the swing still to go.
    struct AimIkSettings
    {
        Float3 target = Float3::Zero; // model space
        Float3 aimAxis{0.0f, 0.0f, 1.0f}; // in the last bone's local space
        Float3 upAxis{0.0f, 1.0f, 0.0f};  // in the last bone's local space
        // The direction the up axis leans toward after the swing; without one, the animated up
        // is kept (no roll drifts in from the swing).
        bool hasUp = false;
        Float3 up = Float3::Zero; // model space
        f32 maxAngle = 60.0f * kDegToRad; // from the animated aim direction
        f32 weight = 1.0f;
    };

    /// The most bones an aim spreads over (its weight blend keeps their poses on the stack).
    inline constexpr usize kMaxAimBones = 16;

    /// How close an end must come to count as reached: this fraction of the chain's length.
    inline constexpr f32 kIkReachTolerance = 1.0e-3f;

    /// The fraction of full reach a two-bone chain stops at, short of locking straight.
    inline constexpr f32 kTwoBoneMaxReach = 0.995f;

    namespace ik
    {
        [[nodiscard]] inline Float3 Position(const Float4x4& m) noexcept
        {
            return Float3{m.m[3][0], m.m[3][1], m.m[3][2]};
        }

        [[nodiscard]] inline Quaternion RotationOf(const Float4x4& m) noexcept
        {
            Float3 translation;
            Float3 scale;
            Quaternion rotation;
            (void)Decompose(m, translation, rotation, scale);
            return rotation;
        }

        /// `v` less its part along unit `axis`.
        [[nodiscard]] inline Float3 Perpendicular(Float3 v, Float3 axis) noexcept
        {
            return v - axis * Dot(v, axis);
        }

        /// A unit vector perpendicular to unit `v`, the same one every time for the same `v`.
        [[nodiscard]] inline Float3 AnyPerpendicular(Float3 v) noexcept
        {
            const Float3 ax{Abs(v.x), Abs(v.y), Abs(v.z)};
            const Float3 other = (ax.x <= ax.y && ax.x <= ax.z)   ? Float3{1.0f, 0.0f, 0.0f}
                                 : (ax.y <= ax.z) ? Float3{0.0f, 1.0f, 0.0f}
                                                  : Float3{0.0f, 0.0f, 1.0f};
            return Normalized(Perpendicular(other, v));
        }

        /// The shortest turn taking unit `from` to unit `to` (a half turn about a fixed
        /// perpendicular when they are opposite).
        [[nodiscard]] inline Quaternion FromTo(Float3 from, Float3 to) noexcept
        {
            const f32 cosine = Dot(from, to);
            if (cosine >= 1.0f - 1.0e-7f)
            {
                return Quaternion::Identity;
            }
            if (cosine <= -1.0f + 1.0e-7f)
            {
                return Quaternion::FromAxisAngle(AnyPerpendicular(from), kPi);
            }
            const Float3 axis = Cross(from, to);
            return Normalized(Quaternion{axis.x, axis.y, axis.z, 1.0f + cosine});
        }

        /// The turn about unit `axis` taking `from` toward `to`, both perpendicular to it.
        [[nodiscard]] inline f32 SignedAngle(Float3 from, Float3 to, Float3 axis) noexcept
        {
            return Atan2(Dot(Cross(from, to), axis), Dot(from, to));
        }

        [[nodiscard]] inline Quaternion ParentModelRotation(const Skeleton& skeleton,
                                                            const ModelPoseCache& model, i32 bone)
        {
            const Bone* b = skeleton.GetBone(bone);
            if (b != nullptr && b->parentIndex >= 0)
            {
                return RotationOf(model.At(b->parentIndex));
            }
            return b != nullptr ? RotationOf(b->rootCorrection) : Quaternion::Identity;
        }

        /// Turns `bone` by the model-space rotation `turn` about its own origin.
        inline void TurnInModel(const Skeleton& skeleton, Span<BoneTransform> local,
                                const ModelPoseCache& model, i32 bone, Quaternion turn)
        {
            const Quaternion parent = ParentModelRotation(skeleton, model, bone);
            BoneTransform& t = local[static_cast<usize>(bone)];
            t.rotation = Normalized(Inverse(parent) * turn * parent * t.rotation);
        }

        inline void Rebuild(const Skeleton& skeleton, Span<BoneTransform> local, ModelPoseCache& model,
                            i32 bone)
        {
            model.RebuildFrom(skeleton, Span<const BoneTransform>{local.Data(), local.Size()}, bone);
        }

        [[nodiscard]] inline bool InPose(const Skeleton& skeleton, Span<BoneTransform> local, i32 bone)
        {
            return bone >= 0 && bone < skeleton.BoneCount() && static_cast<usize>(bone) < local.Size();
        }

        /// True when `ancestor` is above `bone` (not the bone itself).
        [[nodiscard]] inline bool IsBelow(const Skeleton& skeleton, i32 bone, i32 ancestor)
        {
            const Bone* b = skeleton.GetBone(bone);
            for (i32 steps = 0; b != nullptr && b->parentIndex >= 0 && steps <= skeleton.BoneCount();
                 ++steps)
            {
                if (b->parentIndex == ancestor)
                {
                    return true;
                }
                b = skeleton.GetBone(b->parentIndex);
            }
            return false;
        }

        /// The cache is current for this skeleton (a solver may be called outside a stack).
        inline void EnsureModel(const Skeleton& skeleton, Span<BoneTransform> local, ModelPoseCache& model)
        {
            if (model.Model().Size() != static_cast<usize>(skeleton.BoneCount()))
            {
                model.Build(skeleton, Span<const BoneTransform>{local.Data(), local.Size()});
            }
        }

        /// The side the mid joint bends toward, perpendicular to the unit chain direction `along`:
        /// the animated chain's bend, else the bind pose's carried by the start bone's turn since,
        /// else the hinge, else a fixed perpendicular (the same every frame).
        [[nodiscard]] inline Float3 BendSide(const Skeleton& skeleton, const ModelPoseCache& model,
                                             const TwoBoneIkChain& chain, const TwoBoneIkSettings& s,
                                             Float3 along, f32 upperLength)
        {
            const f32 straight = 1.0e-2f * upperLength; // a bend under about half a degree
            const Float3 start = Position(model.At(chain.start));
            const Float3 animated = Perpendicular(Position(model.At(chain.mid)) - start, along);
            if (Length(animated) > straight)
            {
                return Normalized(animated);
            }
            const Quaternion startNow = RotationOf(model.At(chain.start));
            const Bone* bones[3] = {skeleton.GetBone(chain.start), skeleton.GetBone(chain.mid),
                                    skeleton.GetBone(chain.end)};
            const Float4x4 bindStart = Inverse(bones[0]->inverseBindPose);
            const Float3 bindA = Position(bindStart);
            const Float3 bindB = Position(Inverse(bones[1]->inverseBindPose));
            const Float3 bindC = Position(Inverse(bones[2]->inverseBindPose));
            const Float3 bindAlong = Normalized(bindC - bindA);
            const Float3 bindBend = Perpendicular(bindB - bindA, bindAlong);
            if (Length(bindAlong) > 0.5f && Length(bindBend) > straight)
            {
                const Quaternion sinceBind = startNow * Inverse(RotationOf(bindStart));
                const Float3 carried = Perpendicular(RotateVector(sinceBind, bindBend), along);
                if (Length(carried) > straight)
                {
                    return Normalized(carried);
                }
            }
            if (LengthSquared(s.hingeAxis) > 1.0e-12f)
            {
                const Float3 side = Cross(RotateVector(startNow, Normalized(s.hingeAxis)), along);
                if (LengthSquared(side) > 1.0e-8f)
                {
                    return Normalized(Perpendicular(side, along));
                }
            }
            return AnyPerpendicular(along);
        }
    }

    /// Bends a two-bone chain so its end reaches `settings.target`: exactly when the target is in
    /// reach, else stopped at kTwoBoneMaxReach of full reach (or the chain's shortest fold) and
    /// pointed at it. Bone lengths are the pose's own (a rotation keeps them; the bind pose's would
    /// miss when an animation moved a bone). The mid joint stays in the bend plane, so it never
    /// rolls or flips between frames. Writes the start and mid local rotations, and the end's with
    /// matchRotation.
    inline IkResult SolveTwoBone(const Skeleton& skeleton, Span<BoneTransform> local, ModelPoseCache& model,
                                 const TwoBoneIkChain& chain, const TwoBoneIkSettings& settings)
    {
        IkResult result;
        if (!ik::InPose(skeleton, local, chain.start) || !ik::InPose(skeleton, local, chain.mid) ||
            !ik::InPose(skeleton, local, chain.end) || !ik::IsBelow(skeleton, chain.mid, chain.start) ||
            !ik::IsBelow(skeleton, chain.end, chain.mid))
        {
            return result;
        }
        ik::EnsureModel(skeleton, local, model);
        result.valid = true;

        const Float3 a = ik::Position(model.At(chain.start));
        const Float3 b = ik::Position(model.At(chain.mid));
        const Float3 c = ik::Position(model.At(chain.end));
        const Float3 target = settings.target;
        const f32 upper = Length(b - a);
        const f32 lower = Length(c - b);
        const f32 reach = upper + lower;
        if (upper <= kEpsilon || lower <= kEpsilon || settings.weight <= 0.0f)
        {
            result.error = Length(c - target);
            result.reached = result.error <= kIkReachTolerance * reach;
            return result;
        }
        const BoneTransform before[3] = {local[static_cast<usize>(chain.start)],
                                         local[static_cast<usize>(chain.mid)],
                                         local[static_cast<usize>(chain.end)]};

        // 1. Open or close the mid joint, in the chain's bend plane, to the distance it must span.
        Float3 along = c - a;
        along = Length(along) > kEpsilon * reach ? Normalized(along) : Normalized(b - a);
        const Float3 side = ik::BendSide(skeleton, model, chain, settings, along, upper);
        const Float3 hinge = Normalized(Cross(along, side));
        const f32 shortest = Max(Abs(upper - lower), 1.0e-4f * reach);
        const f32 span = Clamp(Length(target - a), shortest, kTwoBoneMaxReach * reach);
        const f32 cosWanted =
            Clamp((upper * upper + lower * lower - span * span) / (2.0f * upper * lower), -1.0f, 1.0f);
        // The interior angle measured about the hinge (a turn about it opens the joint), so a
        // straight chain or one bent the other way still lands on the bend side.
        const f32 now = ik::SignedAngle(Normalized(a - b), Normalized(c - b), hinge);
        ik::TurnInModel(skeleton, local, model, chain.mid,
                        Quaternion::FromAxisAngle(hinge, Acos(cosWanted) - now));
        ik::Rebuild(skeleton, local, model, chain.mid);

        // 2. Swing the whole chain from the start: its end direction onto the target's, and its
        //    bend side onto the pole's (or carried along with the swing, without one).
        const Float3 bent = ik::Position(model.At(chain.end));
        const Float3 alongNow = Normalized(bent - a);
        const Float3 sideNow = Normalized(ik::Perpendicular(b - a, alongNow));
        const Float3 toTarget = Length(target - a) > kEpsilon * reach ? Normalized(target - a) : alongNow;
        const Quaternion swing = ik::FromTo(alongNow, toTarget);
        const Float3 sideSwung = Normalized(ik::Perpendicular(RotateVector(swing, sideNow), toTarget));
        Float3 sideWanted = sideSwung;
        if (settings.hasPole)
        {
            const Float3 toPole = ik::Perpendicular(settings.pole - a, toTarget);
            if (Length(toPole) > 1.0e-4f * reach)
            {
                sideWanted = Normalized(toPole);
            }
        }
        const Quaternion twist =
            Quaternion::FromAxisAngle(toTarget, ik::SignedAngle(sideSwung, sideWanted, toTarget));
        ik::TurnInModel(skeleton, local, model, chain.start, twist * swing);
        ik::Rebuild(skeleton, local, model, chain.start);

        if (settings.matchRotation)
        {
            const Quaternion parent = ik::ParentModelRotation(skeleton, model, chain.end);
            local[static_cast<usize>(chain.end)].rotation =
                Normalized(Inverse(parent) * settings.targetRotation);
            ik::Rebuild(skeleton, local, model, chain.end);
        }

        if (settings.weight < 1.0f)
        {
            const i32 bones[3] = {chain.start, chain.mid, chain.end};
            for (usize i = 0; i < 3; ++i)
            {
                BoneTransform& t = local[static_cast<usize>(bones[i])];
                t.rotation = Slerp(before[i].rotation, t.rotation, settings.weight);
            }
            ik::Rebuild(skeleton, local, model, chain.start);
        }

        result.error = Length(ik::Position(model.At(chain.end)) - target);
        result.reached = result.error <= kIkReachTolerance * reach;
        return result;
    }

    /// Points the last bone's aim axis at `settings.target`, the swing shared along `bones` (root
    /// first): bone i takes `shares[i]` of what is still to go (a spine: 0.3, 0.5, 1.0; the last
    /// share 1 lands it exactly). Empty `shares` gives the last bone all of it. The direction is
    /// held within maxAngle of the animated one, and the up axis is then turned toward `up` (or
    /// back to the animated up) about the aim axis. The error is the angle left to the target.
    inline IkResult SolveAim(const Skeleton& skeleton, Span<BoneTransform> local, ModelPoseCache& model,
                             Span<const i32> bones, Span<const f32> shares, const AimIkSettings& settings)
    {
        IkResult result;
        if (bones.IsEmpty() || bones.Size() > kMaxAimBones ||
            (!shares.IsEmpty() && shares.Size() != bones.Size()) || LengthSquared(settings.aimAxis) < 1.0e-12f)
        {
            return result;
        }
        for (usize i = 0; i < bones.Size(); ++i)
        {
            if (!ik::InPose(skeleton, local, bones[i]) ||
                (i > 0 && !ik::IsBelow(skeleton, bones[i], bones[i - 1])))
            {
                return result;
            }
        }
        ik::EnsureModel(skeleton, local, model);
        result.valid = true;

        const i32 last = bones[bones.Size() - 1];
        const Float3 aimAxis = Normalized(settings.aimAxis);
        const Quaternion animated = ik::RotationOf(model.At(last));
        const Float3 aimFrom = Normalized(RotateVector(animated, aimAxis));
        const Float3 upFrom = RotateVector(animated, settings.upAxis);
        // The direction to aim from `origin`: the target's, held within maxAngle of the animated.
        auto wanted = [&](Float3 origin) -> Float3
        {
            const Float3 toTarget = settings.target - origin;
            if (Length(toTarget) <= kEpsilon)
            {
                return aimFrom;
            }
            const Float3 direction = Normalized(toTarget);
            const f32 angle = Acos(Clamp(Dot(aimFrom, direction), -1.0f, 1.0f));
            if (angle <= settings.maxAngle)
            {
                return direction;
            }
            Float3 axis = Cross(aimFrom, direction);
            axis = LengthSquared(axis) > 1.0e-12f ? Normalized(axis) : ik::AnyPerpendicular(aimFrom);
            return RotateVector(Quaternion::FromAxisAngle(axis, settings.maxAngle), aimFrom);
        };
        auto aimNow = [&]() { return Normalized(RotateVector(ik::RotationOf(model.At(last)), aimAxis)); };

        auto missed = [&]()
        {
            const Float3 toTarget = settings.target - ik::Position(model.At(last));
            return Length(toTarget) > kEpsilon ? Acos(Clamp(Dot(aimNow(), Normalized(toTarget)), -1.0f, 1.0f))
                                               : 0.0f;
        };
        if (settings.weight <= 0.0f)
        {
            result.error = missed();
            result.reached = result.error <= kIkReachTolerance;
            return result;
        }

        BoneTransform before[kMaxAimBones];
        for (usize i = 0; i < bones.Size(); ++i)
        {
            before[i] = local[static_cast<usize>(bones[i])];
        }
        for (usize i = 0; i < bones.Size(); ++i)
        {
            const f32 share = shares.IsEmpty() ? (i + 1 == bones.Size() ? 1.0f : 0.0f) : Clamp(shares[i], 0.0f, 1.0f);
            if (share <= 0.0f)
            {
                continue;
            }
            // The target direction from where the last bone is NOW (a turn above it moved it).
            const Float3 direction = wanted(ik::Position(model.At(last)));
            const Quaternion turn = Slerp(Quaternion::Identity, ik::FromTo(aimNow(), direction), share);
            ik::TurnInModel(skeleton, local, model, bones[i], turn);
            ik::Rebuild(skeleton, local, model, bones[i]);
        }

        // Roll the last bone about its aim so the up axis leans where it should.
        const Float3 aim = aimNow();
        const Float3 upWanted = ik::Perpendicular(settings.hasUp ? settings.up : upFrom, aim);
        const Float3 upHas = ik::Perpendicular(RotateVector(ik::RotationOf(model.At(last)), settings.upAxis), aim);
        if (LengthSquared(upWanted) > 1.0e-10f && LengthSquared(upHas) > 1.0e-10f)
        {
            const f32 roll = ik::SignedAngle(Normalized(upHas), Normalized(upWanted), aim);
            ik::TurnInModel(skeleton, local, model, last, Quaternion::FromAxisAngle(aim, roll));
            ik::Rebuild(skeleton, local, model, last);
        }

        if (settings.weight < 1.0f)
        {
            for (usize i = 0; i < bones.Size(); ++i)
            {
                BoneTransform& t = local[static_cast<usize>(bones[i])];
                t.rotation = Slerp(before[i].rotation, t.rotation, settings.weight);
            }
            ik::Rebuild(skeleton, local, model, bones[0]);
        }

        result.error = missed();
        result.reached = result.error <= kIkReachTolerance;
        return result;
    }
}
