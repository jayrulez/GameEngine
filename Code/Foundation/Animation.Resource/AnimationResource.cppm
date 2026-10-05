// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Animation.Resource - the `foundation.animation.resource` module.
///
/// Skeletons + animation clips as resources: a cooked SkeletonSource / AnimationClipSource
/// (ISerializable - flat parallel arrays) is built by a factory into the runtime
/// animation::Skeleton / animation::AnimationClip. Mirrors foundation.geometry.resource
/// (Source -> Factory -> Product). The graph resource (composite, references clips) lands later.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.animation.resource;

import foundation.core;
import foundation.resource;
import foundation.content;
import foundation.animation;

using namespace foundation::core;
namespace resource = foundation::resource;

export namespace foundation::animation
{

    // ---- skeleton ------------------------------------------------------------------------------

    // Cooked skeleton: per-bone parallel arrays (name, parent, bind TRS, inverse bind matrix).
    class SkeletonSource : public ISerializable
    {
        RTTI_OBJECT(SkeletonSource, ISerializable)
    public:
        String name;
        Array<String> boneNames;
        Array<i32> parentIndices;
        Array<Float3> translations;
        Array<Quaternion> rotations;
        Array<Float3> scales;
        Array<Float4x4> inverseBindPoses;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "boneNames", boneNames);
            foundation::core::Serialize(ar, "parentIndices", parentIndices);
            foundation::core::Serialize(ar, "translations", translations);
            foundation::core::Serialize(ar, "rotations", rotations);
            foundation::core::Serialize(ar, "scales", scales);
            foundation::core::Serialize(ar, "inverseBindPoses", inverseBindPoses);
        }

        // Capture a runtime Skeleton into this source (for cooking).
        static void FromSkeleton(const Skeleton& skel, SkeletonSource& out)
        {
            out.name = String(skel.Name().AsView());
            out.boneNames.Clear();
            out.parentIndices.Clear();
            out.translations.Clear();
            out.rotations.Clear();
            out.scales.Clear();
            out.inverseBindPoses.Clear();
            for (const Bone& b : skel.Bones())
            {
                out.boneNames.PushBack(String(b.name.AsView()));
                out.parentIndices.PushBack(b.parentIndex);
                out.translations.PushBack(b.localBindPose.position);
                out.rotations.PushBack(b.localBindPose.rotation);
                out.scales.PushBack(b.localBindPose.scale);
                out.inverseBindPoses.PushBack(b.inverseBindPose);
            }
        }

        // Populate a runtime Skeleton from this source (rebuilds name map + hierarchy).
        void FillSkeleton(Skeleton& skel) const
        {
            const i32 count = static_cast<i32>(parentIndices.Size());
            skel.ClearForReload(count);
            skel.Name() = String(name.AsView());
            Array<Bone>& bones = skel.Bones();
            for (usize i = 0; i < bones.Size(); ++i)
            {
                bones[i].index = static_cast<i32>(i);
                bones[i].parentIndex = parentIndices[i];
                if (i < boneNames.Size())
                {
                    bones[i].name = String(boneNames[i].AsView());
                }
                bones[i].localBindPose =
                    BoneTransform{i < translations.Size() ? translations[i] : Float3{0, 0, 0},
                                  i < rotations.Size() ? rotations[i] : Quaternion::Identity,
                                  i < scales.Size() ? scales[i] : Float3{1, 1, 1}};
                if (i < inverseBindPoses.Size())
                {
                    bones[i].inverseBindPose = inverseBindPoses[i];
                }
            }
            skel.BuildNameMap();
            skel.FindRootBones();
            skel.BuildChildIndices();
        }
    };

    class SkeletonFactory final : public resource::IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required -
        // the application that registers the factory decides).
        explicit SkeletonFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &Skeleton::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &SkeletonSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            (void)manager;
            RefPtr<ISerializable> object = instance.ReadObject();
            SkeletonSource* src = Cast<SkeletonSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<Skeleton> skel = MakeRef<Skeleton>((*m_allocator));
            src->FillSkeleton(*skel);
            return skel;
        }
    
    private:
        IAllocator* m_allocator;
    };

    // ---- animation clip ------------------------------------------------------------------------

    // What a clip extracts for root motion (root-motion.md), authored on the clip: the root (a bone
    // name; empty = the armature's own channels when the clip has them, else the skeleton's first
    // root) and which parts of its travel. Every part is OFF by default.
    struct RootMotionSettings
    {
        String rootBone;
        bool horizontal = false; // the ground-plane translation
        bool vertical = false;   // height (a climb; off for a walk, which keeps its bob)
        bool yaw = false;        // the turn about up (never pitch or roll)

        [[nodiscard]] bool Any() const noexcept { return horizontal || vertical || yaw; }
    };

    // Cooked clip: per-track metadata + a dense keyframe pool (times + Float4 values: xyz for
    // position/scale, xyzw for rotation), plus events. A track whose bone is -1 is a MODEL track:
    // the armature node's own channels (the importer keeps them for root motion); the pose never
    // plays it. Appended last: the root motion settings and the curve the cook baked.
    class AnimationClipSource : public ISerializable
    {
        RTTI_OBJECT(AnimationClipSource, ISerializable)
    public:
        enum class TrackKind : u8
        {
            Position = 0,
            Rotation = 1,
            Scale = 2
        };

        String name;
        f32 duration = 0.0f;
        bool isLooping = false;
        Array<i32> trackBone;  // bone index per track
        Array<u8> trackKind;   // TrackKind
        Array<u8> trackInterp; // InterpolationMode
        Array<u32> trackStart; // first keyframe index into keyTimes/keyValues
        Array<u32> trackCount; // keyframe count
        Array<f32> keyTimes;
        Array<Float4> keyValues; // position/scale in xyz; rotation in xyzw
        Array<f32> eventTimes;
        Array<String> eventNames;
        RootMotionSettings rootMotion;
        Array<f32> rootTimes;       // baked at cook (empty unless rootMotion.Any())
        Array<Float3> rootPositions;
        Array<f32> rootYaws;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "duration", duration);
            foundation::core::Serialize(ar, "isLooping", isLooping);
            foundation::core::Serialize(ar, "trackBone", trackBone);
            foundation::core::Serialize(ar, "trackKind", trackKind);
            foundation::core::Serialize(ar, "trackInterp", trackInterp);
            foundation::core::Serialize(ar, "trackStart", trackStart);
            foundation::core::Serialize(ar, "trackCount", trackCount);
            foundation::core::Serialize(ar, "keyTimes", keyTimes);
            foundation::core::Serialize(ar, "keyValues", keyValues);
            foundation::core::Serialize(ar, "eventTimes", eventTimes);
            foundation::core::Serialize(ar, "eventNames", eventNames);
            // Appended (root-motion.md P0): a text clip from before them reads with root motion
            // off. Cooked (binary, positional) clips re-cook: the clip builder's version moved.
            // Each its own key: a structure writes its fields flat, with no key of its own to test.
            SerializeAppended(ar, "rootBone", rootMotion.rootBone);
            SerializeAppended(ar, "rootHorizontal", rootMotion.horizontal);
            SerializeAppended(ar, "rootVertical", rootMotion.vertical);
            SerializeAppended(ar, "rootYaw", rootMotion.yaw);
            SerializeAppended(ar, "rootTimes", rootTimes);
            SerializeAppended(ar, "rootPositions", rootPositions);
            SerializeAppended(ar, "rootYaws", rootYaws);
        }

        /// Every field of `other` (a cook's working copy: the builder bakes and strips it, the
        /// authored asset stays as it was).
        void CopyFrom(const AnimationClipSource& other)
        {
            name = other.name;
            duration = other.duration;
            isLooping = other.isLooping;
            trackBone = other.trackBone;
            trackKind = other.trackKind;
            trackInterp = other.trackInterp;
            trackStart = other.trackStart;
            trackCount = other.trackCount;
            keyTimes = other.keyTimes;
            keyValues = other.keyValues;
            eventTimes = other.eventTimes;
            eventNames = other.eventNames;
            rootMotion = other.rootMotion;
            rootTimes = other.rootTimes;
            rootPositions = other.rootPositions;
            rootYaws = other.rootYaws;
        }

        static void FromClip(const AnimationClip& clip, AnimationClipSource& out)
        {
            out.name = String(clip.Name().AsView());
            out.duration = clip.duration;
            out.isLooping = clip.isLooping;
            out.trackBone.Clear();
            out.trackKind.Clear();
            out.trackInterp.Clear();
            out.trackStart.Clear();
            out.trackCount.Clear();
            out.keyTimes.Clear();
            out.keyValues.Clear();
            for (const auto& t : clip.PositionTracks())
            {
                AppendVec3Track(out, *t, TrackKind::Position);
            }
            for (const auto& t : clip.RotationTracks())
            {
                AppendQuatTrack(out, *t);
            }
            for (const auto& t : clip.ScaleTracks())
            {
                AppendVec3Track(out, *t, TrackKind::Scale);
            }
            out.eventTimes.Clear();
            out.eventNames.Clear();
            for (const AnimationEvent& e : clip.Events())
            {
                out.eventTimes.PushBack(e.time);
                out.eventNames.PushBack(String(e.name.AsView()));
            }
            out.rootMotion.horizontal = clip.rootMotion.horizontal;
            out.rootMotion.vertical = clip.rootMotion.vertical;
            out.rootMotion.yaw = clip.rootMotion.yaw;
            out.rootTimes = clip.rootMotion.times;
            out.rootPositions = clip.rootMotion.positions;
            out.rootYaws = clip.rootMotion.yaws;
        }

        /// Rebuilds `clip` in place. False - and the clip left EMPTY - when the record is
        /// malformed: a track table shorter than its bone list, or a keyframe run past the
        /// pool. Cooked data is ours, but a truncated or corrupt record must refuse, not read
        /// past the end of an array.
        [[nodiscard]] bool FillClip(AnimationClip& clip) const
        {
            clip.ClearForReload();
            const usize trackTotal = trackBone.Size();
            if (trackStart.Size() < trackTotal || trackCount.Size() < trackTotal)
            {
                return false;
            }
            const usize pool = Min(keyTimes.Size(), keyValues.Size());
            for (usize i = 0; i < trackTotal; ++i)
            {
                const usize start = trackStart[i];
                const usize count = trackCount[i];
                if (start > pool || count > pool - start)
                {
                    return false;
                }
            }
            clip.Name() = String(name.AsView());
            clip.duration = duration;
            clip.isLooping = isLooping;
            for (usize i = 0; i < trackTotal; ++i)
            {
                if (trackBone[i] < 0)
                {
                    continue; // a model track: root motion's (baked into the curve), never the pose's
                }
                const TrackKind kind =
                    static_cast<TrackKind>(i < trackKind.Size() ? trackKind[i] : 0);
                const InterpolationMode interp =
                    static_cast<InterpolationMode>(i < trackInterp.Size() ? trackInterp[i] : 1);
                const u32 start = trackStart[i];
                const u32 count = trackCount[i];
                if (kind == TrackKind::Rotation)
                {
                    AnimationTrack<Quaternion>* track = clip.GetOrCreateRotationTrack(trackBone[i]);
                    track->interpolation = interp;
                    for (u32 j = 0; j < count; ++j)
                    {
                        const Float4 v = keyValues[start + j];
                        track->AddKeyframe(keyTimes[start + j], Quaternion{v.x, v.y, v.z, v.w});
                    }
                }
                else
                {
                    AnimationTrack<Float3>* track =
                        (kind == TrackKind::Scale) ? clip.GetOrCreateScaleTrack(trackBone[i])
                                                   : clip.GetOrCreatePositionTrack(trackBone[i]);
                    track->interpolation = interp;
                    for (u32 j = 0; j < count; ++j)
                    {
                        const Float4 v = keyValues[start + j];
                        track->AddKeyframe(keyTimes[start + j], Float3{v.x, v.y, v.z});
                    }
                }
            }
            for (usize i = 0; i < eventTimes.Size(); ++i)
            {
                clip.AddEvent(eventTimes[i],
                              (i < eventNames.Size()) ? eventNames[i].AsView() : StringView{});
            }
            const usize roots = Min(rootTimes.Size(), Min(rootPositions.Size(), rootYaws.Size()));
            for (usize i = 0; i < roots; ++i)
            {
                clip.rootMotion.times.PushBack(rootTimes[i]);
                clip.rootMotion.positions.PushBack(rootPositions[i]);
                clip.rootMotion.yaws.PushBack(rootYaws[i]);
            }
            clip.rootMotion.horizontal = roots > 0 && rootMotion.horizontal;
            clip.rootMotion.vertical = roots > 0 && rootMotion.vertical;
            clip.rootMotion.yaw = roots > 0 && rootMotion.yaw;
            return true;
        }

    private:
        static void AppendVec3Track(AnimationClipSource& out, const AnimationTrack<Float3>& track,
                                    TrackKind kind)
        {
            out.trackBone.PushBack(track.boneIndex);
            out.trackKind.PushBack(static_cast<u8>(kind));
            out.trackInterp.PushBack(static_cast<u8>(track.interpolation));
            out.trackStart.PushBack(static_cast<u32>(out.keyTimes.Size()));
            out.trackCount.PushBack(static_cast<u32>(track.Keyframes().Size()));
            for (const Keyframe<Float3>& k : track.Keyframes())
            {
                out.keyTimes.PushBack(k.time);
                out.keyValues.PushBack(Float4{k.value.x, k.value.y, k.value.z, 0.0f});
            }
        }
        static void AppendQuatTrack(AnimationClipSource& out,
                                    const AnimationTrack<Quaternion>& track)
        {
            out.trackBone.PushBack(track.boneIndex);
            out.trackKind.PushBack(static_cast<u8>(TrackKind::Rotation));
            out.trackInterp.PushBack(static_cast<u8>(track.interpolation));
            out.trackStart.PushBack(static_cast<u32>(out.keyTimes.Size()));
            out.trackCount.PushBack(static_cast<u32>(track.Keyframes().Size()));
            for (const Keyframe<Quaternion>& k : track.Keyframes())
            {
                out.keyTimes.PushBack(k.time);
                out.keyValues.PushBack(Float4{k.value.x, k.value.y, k.value.z, k.value.w});
            }
        }
    };

    /// The rate root motion is baked at, beside the root's own keys: a cubic or step track keeps
    /// its shape between sparse keys (the runtime reads the curve linearly).
    inline constexpr f32 kRootMotionBakeRate = 30.0f;

    namespace root_motion
    {
        [[nodiscard]] inline f32 YawOf(Quaternion q) noexcept
        {
            const Float3 forward = RotateVector(q, Float3{0.0f, 0.0f, 1.0f});
            return Atan2(forward.x, forward.z);
        }

        /// The clip's track of `kind` for `bone` (-1: a model track), or -1.
        [[nodiscard]] inline i32 FindTrack(const AnimationClipSource& clip, i32 bone,
                                           AnimationClipSource::TrackKind kind) noexcept
        {
            for (usize i = 0; i < clip.trackBone.Size() && i < clip.trackKind.Size(); ++i)
            {
                if (clip.trackBone[i] == bone && clip.trackKind[i] == static_cast<u8>(kind))
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        /// One track of the record as a runtime track (sampled exactly as the pose samples it).
        inline void ReadTrack(const AnimationClipSource& clip, i32 track, AnimationTrack<Float3>& out)
        {
            if (track < 0)
            {
                return;
            }
            const usize t = static_cast<usize>(track);
            out.interpolation = static_cast<InterpolationMode>(t < clip.trackInterp.Size() ? clip.trackInterp[t] : 1);
            for (u32 k = 0; k < clip.trackCount[t] && clip.trackStart[t] + k < clip.keyTimes.Size(); ++k)
            {
                const Float4 v = clip.keyValues[clip.trackStart[t] + k];
                out.AddKeyframe(clip.keyTimes[clip.trackStart[t] + k], Float3{v.x, v.y, v.z});
            }
        }
        inline void ReadTrack(const AnimationClipSource& clip, i32 track, AnimationTrack<Quaternion>& out)
        {
            if (track < 0)
            {
                return;
            }
            const usize t = static_cast<usize>(track);
            out.interpolation = static_cast<InterpolationMode>(t < clip.trackInterp.Size() ? clip.trackInterp[t] : 1);
            for (u32 k = 0; k < clip.trackCount[t] && clip.trackStart[t] + k < clip.keyTimes.Size(); ++k)
            {
                const Float4 v = clip.keyValues[clip.trackStart[t] + k];
                out.AddKeyframe(clip.keyTimes[clip.trackStart[t] + k], Quaternion{v.x, v.y, v.z, v.w});
            }
        }
    }

    /// The root motion cook (root-motion.md P0): bakes the root's travel into the clip's curve and,
    /// for a root BONE, strips what was extracted from its own tracks so the pose plays in place,
    /// keeping frame 0 (a walk keeps its bob when `vertical` is off). `root` is a bone index in the
    /// clip's tracks, or -1 for the armature's own channels (model tracks: converted into model
    /// space by the inverse of the armature's rest transform `modelRest`, and never stripped, as the
    /// pose never plays them). The curve is the root's transform in its parent's space, which is
    /// model space for a skeleton root and for a model track; for a root under a parent it is model
    /// space only while its ancestors neither move nor turn (positions and yaw alike). Up is +Y;
    /// pitch and roll are never extracted. False, and nothing changed, when the root has no track.
    inline bool BakeRootMotion(AnimationClipSource& clip, i32 root, const Transform& modelRest = Transform{})
    {
        using Kind = AnimationClipSource::TrackKind;
        clip.rootTimes.Clear();
        clip.rootPositions.Clear();
        clip.rootYaws.Clear();
        if (!clip.rootMotion.Any())
        {
            return true;
        }
        const i32 positionTrack = root_motion::FindTrack(clip, root, Kind::Position);
        const i32 rotationTrack = root_motion::FindTrack(clip, root, Kind::Rotation);
        if (positionTrack < 0 && rotationTrack < 0)
        {
            return false;
        }
        AnimationTrack<Float3> position;
        AnimationTrack<Quaternion> rotation;
        root_motion::ReadTrack(clip, positionTrack, position);
        root_motion::ReadTrack(clip, rotationTrack, rotation);

        // The times: the root's keys and a fixed rate across the clip, in order, once each.
        Array<f32> times;
        const f32 duration = Max(clip.duration, 0.0f);
        const u32 steps = static_cast<u32>(Ceil(duration * kRootMotionBakeRate));
        for (u32 i = 0; i <= steps; ++i)
        {
            times.PushBack(Min(duration, static_cast<f32>(i) / kRootMotionBakeRate));
        }
        for (const Keyframe<Float3>& k : position.Keyframes())
        {
            times.PushBack(Clamp(k.time, 0.0f, duration));
        }
        for (const Keyframe<Quaternion>& k : rotation.Keyframes())
        {
            times.PushBack(Clamp(k.time, 0.0f, duration));
        }
        times.Sort([](f32 a, f32 b) { return a < b; });
        const Float4x4 fromRest = Inverse(modelRest.ToMatrix());
        f32 previousYaw = 0.0f;
        for (usize i = 0; i < times.Size(); ++i)
        {
            if (i > 0 && times[i] - times[i - 1] < 1.0e-5f)
            {
                continue;
            }
            Float3 p = SampleVec3(&position, times[i], Float3::Zero);
            Quaternion q = SampleQuat(&rotation, times[i], Quaternion::Identity);
            if (root < 0)
            {
                // The armature moved within its parent; model space is the armature at rest.
                BoneTransform moved;
                moved.position = p;
                moved.rotation = q;
                Float3 scale;
                (void)Decompose(moved.ToMatrix() * fromRest, p, q, scale);
            }
            // Yaw unwrapped, so a turn past half a circle keeps counting.
            f32 yaw = root_motion::YawOf(q);
            if (!clip.rootTimes.IsEmpty())
            {
                while (yaw - previousYaw > kPi)
                {
                    yaw -= kTwoPi;
                }
                while (yaw - previousYaw < -kPi)
                {
                    yaw += kTwoPi;
                }
            }
            previousYaw = yaw;
            clip.rootTimes.PushBack(times[i]);
            clip.rootPositions.PushBack(p);
            clip.rootYaws.PushBack(yaw);
        }

        if (root >= 0)
        {
            // Strip from the bone's own keys what was extracted, relative to frame 0.
            const Float3 first = clip.rootPositions[0];
            const f32 firstYaw = clip.rootYaws[0];
            if (positionTrack >= 0 && (clip.rootMotion.horizontal || clip.rootMotion.vertical))
            {
                const usize t = static_cast<usize>(positionTrack);
                for (u32 k = 0; k < clip.trackCount[t]; ++k)
                {
                    Float4& v = clip.keyValues[clip.trackStart[t] + k];
                    if (clip.rootMotion.horizontal)
                    {
                        v.x = first.x;
                        v.z = first.z;
                    }
                    if (clip.rootMotion.vertical)
                    {
                        v.y = first.y;
                    }
                }
            }
            if (rotationTrack >= 0 && clip.rootMotion.yaw)
            {
                const usize t = static_cast<usize>(rotationTrack);
                for (u32 k = 0; k < clip.trackCount[t]; ++k)
                {
                    Float4& v = clip.keyValues[clip.trackStart[t] + k];
                    const Quaternion q{v.x, v.y, v.z, v.w};
                    const Quaternion back = Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f},
                                                                      -(root_motion::YawOf(q) - firstYaw));
                    const Quaternion kept = Normalized(back * q);
                    v = Float4{kept.x, kept.y, kept.z, kept.w};
                }
            }
        }
        return true;
    }


    class AnimationClipFactory final : public resource::IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required -
        // the application that registers the factory decides).
        explicit AnimationClipFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AnimationClip::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &AnimationClipSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            (void)manager;
            RefPtr<ISerializable> object = instance.ReadObject();
            AnimationClipSource* src = Cast<AnimationClipSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<AnimationClip> clip = MakeRef<AnimationClip>((*m_allocator));
            if (!src->FillClip(*clip))
            {
                return RefPtr<Object>{}; // a malformed record binds nothing, loudly
            }
            return clip;
        }
    
    private:
        IAllocator* m_allocator;
    };

    // ---- animation graph (authored, composite) -------------------------------------------------
    // The graph is authored directly (its source IS the authoritative form, unlike skeleton/clip which
    // are captured from models). The factory builds a runtime AnimationGraph, resolving each clip
    // reference (Guid) via manager.Bind<AnimationClip> - which records a dependency edge, so the manager
    // keeps the clips alive while the graph is bound (the graph's nodes hold borrowed AnimationClip*).

    // Authored sub-records - plain copyable value structs (so they live in Array<T>), each with a free
    // Serialize overload found by ADL from the Array<T> serializer. (Not ISerializable, which is
    // non-copyable.) One state node: a clip, or a 1D/2D blend tree; clips referenced by resource id.
    struct GraphNodeData
    {
        u8 kind = 0;                            // 0 = clip, 1 = blend1d, 2 = blend2d
        Guid clipRef;                           // clip node
        i32 paramIndex = -1;                    // blend1d driver
        i32 paramIndexX = -1, paramIndexY = -1; // blend2d drivers
        Array<f32> entryThresholds;             // blend1d, parallel to entryClips
        Array<Float2> entryPositions;           // blend2d, parallel to entryClips
        Array<Guid> entryClips;
    };
    inline void Serialize(ISerializer& ar, GraphNodeData& n)
    {
        foundation::core::Serialize(ar, "kind", n.kind);
        foundation::core::Serialize(ar, "clipRef", n.clipRef);
        foundation::core::Serialize(ar, "paramIndex", n.paramIndex);
        foundation::core::Serialize(ar, "paramIndexX", n.paramIndexX);
        foundation::core::Serialize(ar, "paramIndexY", n.paramIndexY);
        foundation::core::Serialize(ar, "entryThresholds", n.entryThresholds);
        foundation::core::Serialize(ar, "entryPositions", n.entryPositions);
        foundation::core::Serialize(ar, "entryClips", n.entryClips);
    }

    struct GraphConditionData
    {
        i32 paramIndex = 0;
        u8 op = 0;
        f32 threshold = 0.0f;
    };
    inline void Serialize(ISerializer& ar, GraphConditionData& c)
    {
        foundation::core::Serialize(ar, "paramIndex", c.paramIndex);
        foundation::core::Serialize(ar, "op", c.op);
        foundation::core::Serialize(ar, "threshold", c.threshold);
    }

    struct GraphTransitionData
    {
        i32 src = -1, dst = 0;
        f32 duration = 0.25f;
        bool hasExitTime = false;
        f32 exitTime = 1.0f;
        i32 priority = 0;
        Array<GraphConditionData> conditions;
    };
    inline void Serialize(ISerializer& ar, GraphTransitionData& t)
    {
        foundation::core::Serialize(ar, "src", t.src);
        foundation::core::Serialize(ar, "dst", t.dst);
        foundation::core::Serialize(ar, "duration", t.duration);
        foundation::core::Serialize(ar, "hasExitTime", t.hasExitTime);
        foundation::core::Serialize(ar, "exitTime", t.exitTime);
        foundation::core::Serialize(ar, "priority", t.priority);
        foundation::core::Serialize(ar, "conditions", t.conditions);
    }

    struct GraphStateData
    {
        String name;
        f32 speed = 1.0f;
        bool loop = true;
        GraphNodeData node;
    };
    inline void Serialize(ISerializer& ar, GraphStateData& s)
    {
        foundation::core::Serialize(ar, "name", s.name);
        foundation::core::Serialize(ar, "speed", s.speed);
        foundation::core::Serialize(ar, "loop", s.loop);
        foundation::core::Serialize(ar, "node", s.node);
    }

    struct GraphLayerData
    {
        String name;
        i32 defaultState = 0;
        u8 blendMode = 0;
        f32 weight = 1.0f;
        Array<f32> maskWeights; // empty = no mask
        Array<GraphStateData> states;
        Array<GraphTransitionData> transitions;
    };
    inline void Serialize(ISerializer& ar, GraphLayerData& l)
    {
        foundation::core::Serialize(ar, "name", l.name);
        foundation::core::Serialize(ar, "defaultState", l.defaultState);
        foundation::core::Serialize(ar, "blendMode", l.blendMode);
        foundation::core::Serialize(ar, "weight", l.weight);
        foundation::core::Serialize(ar, "maskWeights", l.maskWeights);
        foundation::core::Serialize(ar, "states", l.states);
        foundation::core::Serialize(ar, "transitions", l.transitions);
    }

    // Cooked graph: parameters (name/type + default values) + layers (states/transitions/mask).
    class AnimationGraphSource : public ISerializable
    {
        RTTI_OBJECT(AnimationGraphSource, ISerializable)
    public:
        Array<String> paramNames;
        Array<u8> paramTypes;   // AnimationParameterType
        Array<f32> paramFloats; // default values (per parameter, parallel)
        Array<i32> paramInts;
        Array<u8> paramBools;
        Array<GraphLayerData> layers;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "paramNames", paramNames);
            foundation::core::Serialize(ar, "paramTypes", paramTypes);
            foundation::core::Serialize(ar, "paramFloats", paramFloats);
            foundation::core::Serialize(ar, "paramInts", paramInts);
            foundation::core::Serialize(ar, "paramBools", paramBools);
            foundation::core::Serialize(ar, "layers", layers);
        }

        // Build a runtime AnimationGraph, resolving clip references through the manager.
        void BuildInto(resource::ResourceManager& manager, AnimationGraph& graph) const
        {
            for (usize i = 0; i < paramNames.Size(); ++i)
            {
                const auto type =
                    static_cast<AnimationParameterType>(i < paramTypes.Size() ? paramTypes[i] : 0);
                const i32 idx = graph.AddParameter(paramNames[i].AsView(), type);
                if (AnimationGraphParameter* p = graph.GetParameter(idx))
                {
                    if (i < paramFloats.Size())
                    {
                        p->floatValue = paramFloats[i];
                    }
                    if (i < paramInts.Size())
                    {
                        p->intValue = paramInts[i];
                    }
                    if (i < paramBools.Size())
                    {
                        p->boolValue = paramBools[i] != 0;
                    }
                }
            }
            for (const GraphLayerData& ld : layers)
            {
                UniquePtr<AnimationLayer> layer =
                    MakeUnique<AnimationLayer>(graph.MemoryAllocator(), ld.name.AsView());
                layer->defaultStateIndex = ld.defaultState;
                layer->blendMode = static_cast<LayerBlendMode>(ld.blendMode);
                layer->weight = ld.weight;
                if (!ld.maskWeights.IsEmpty())
                {
                    UniquePtr<BoneMask> mask = MakeUnique<BoneMask>(
                        graph.MemoryAllocator(), static_cast<i32>(ld.maskWeights.Size()), 0.0f);
                    for (usize b = 0; b < ld.maskWeights.Size(); ++b)
                    {
                        mask->SetWeight(static_cast<i32>(b), ld.maskWeights[b]);
                    }
                    layer->SetMask(static_cast<UniquePtr<BoneMask>&&>(mask));
                }
                for (const GraphStateData& sd : ld.states)
                {
                    UniquePtr<AnimationGraphState> state = MakeUnique<AnimationGraphState>(
                        graph.MemoryAllocator(), sd.name.AsView(), BuildNode(manager, graph.MemoryAllocator(), sd.node));
                    state->speed = sd.speed;
                    state->loop = sd.loop;
                    layer->AddState(static_cast<UniquePtr<AnimationGraphState>&&>(state));
                }
                for (const GraphTransitionData& td : ld.transitions)
                {
                    UniquePtr<AnimationGraphTransition> tr =
                        MakeUnique<AnimationGraphTransition>(graph.MemoryAllocator());
                    tr->sourceStateIndex = td.src;
                    tr->destStateIndex = td.dst;
                    tr->duration = td.duration;
                    tr->hasExitTime = td.hasExitTime;
                    tr->exitTime = td.exitTime;
                    tr->priority = td.priority;
                    for (const GraphConditionData& cd : td.conditions)
                    {
                        tr->Conditions().PushBack(AnimationGraphCondition{
                            cd.paramIndex, static_cast<ComparisonOp>(cd.op), cd.threshold});
                    }
                    layer->AddTransition(static_cast<UniquePtr<AnimationGraphTransition>&&>(tr));
                }
                graph.AddLayer(static_cast<UniquePtr<AnimationLayer>&&>(layer));
            }
        }

    private:
        // Resolve a clip reference (null id -> null clip). Bind records the graph->clip dependency edge.
        [[nodiscard]] static AnimationClip* ResolveClip(resource::ResourceManager& manager,
                                                        const Guid& id)
        {
            return id.IsNil() ? nullptr : manager.Bind<AnimationClip>(id).Get();
        }
        [[nodiscard]] static UniquePtr<IAnimationStateNode>
        BuildNode(resource::ResourceManager& manager, IAllocator& allocator,
                  const GraphNodeData& n)
        {
            if (n.kind == 1)
            { // blend1d
                UniquePtr<BlendTree1D> t = MakeUnique<BlendTree1D>(allocator);
                t->parameterIndex = n.paramIndex;
                for (usize i = 0; i < n.entryClips.Size(); ++i)
                {
                    t->AddEntry(i < n.entryThresholds.Size() ? n.entryThresholds[i] : 0.0f,
                                ResolveClip(manager, n.entryClips[i]));
                }
                return t;
            }
            if (n.kind == 2)
            { // blend2d
                UniquePtr<BlendTree2D> t = MakeUnique<BlendTree2D>(allocator);
                t->parameterIndexX = n.paramIndexX;
                t->parameterIndexY = n.paramIndexY;
                for (usize i = 0; i < n.entryClips.Size(); ++i)
                {
                    t->AddEntry(i < n.entryPositions.Size() ? n.entryPositions[i] : Float2{0, 0},
                                ResolveClip(manager, n.entryClips[i]));
                }
                return t;
            }
            return MakeUnique<ClipStateNode>(allocator,
                                             ResolveClip(manager, n.clipRef)); // clip node
        }
    };

    class AnimationGraphFactory final : public resource::IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required -
        // the application that registers the factory decides).
        explicit AnimationGraphFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AnimationGraph::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &AnimationGraphSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            AnimationGraphSource* src = Cast<AnimationGraphSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<AnimationGraph> graph = MakeRef<AnimationGraph>((*m_allocator));
            src->BuildInto(manager, *graph);
            return graph;
        }
    
    private:
        IAllocator* m_allocator;
    };

    RTTI_DEFINE_OBJECT(SkeletonSource, "rtti::animation")
    RTTI_DEFINE_OBJECT(AnimationClipSource, "rtti::animation")
    RTTI_DEFINE_OBJECT(AnimationGraphSource, "rtti::animation")

} // namespace foundation::animation

export namespace foundation::animation
{
    /// Registers the cooked records + products (content-DB construction by type name). Idempotent.
    inline void RegisterAnimationResourceTypes()
    {
        GlobalTypeRegistry().Register(SkeletonSource::StaticType());
        RegisterSerializable<SkeletonSource>();
        GlobalTypeRegistry().Register(Skeleton::StaticType());
        GlobalTypeRegistry().Register(AnimationClipSource::StaticType());
        RegisterSerializable<AnimationClipSource>();
        GlobalTypeRegistry().Register(AnimationClip::StaticType());
        GlobalTypeRegistry().Register(AnimationGraphSource::StaticType());
        RegisterSerializable<AnimationGraphSource>();
        GlobalTypeRegistry().Register(AnimationGraph::StaticType());
    }

    /// The animation resource module (engine-composition.md D1): the module the engine
    /// composition composes this library's factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kAnimationResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<Skeleton, SkeletonSource, SkeletonFactory>(),
        foundation::resource::FactoryWithAllocator<AnimationClip, AnimationClipSource, AnimationClipFactory>(),
        foundation::resource::FactoryWithAllocator<AnimationGraph, AnimationGraphSource, AnimationGraphFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kAnimationResourceModule{
        u8"animation", &RegisterAnimationResourceTypes, kAnimationResourceFactories,
        sizeof(kAnimationResourceFactories) / sizeof(kAnimationResourceFactories[0])};
}
