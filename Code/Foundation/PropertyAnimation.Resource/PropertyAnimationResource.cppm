// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::PropertyAnimation.Resource - `foundation.propertyanimation.resource`.
//
// The cooked runtime resource and its loader, shaped like Animation.Resource and Texture.Resource:
//   * PropertyAnimationClipResource: the runtime product (a RefCounted Object wrapping the
//     evaluate-ready PropertyAnimationClip), what a component's Ref<> binds and what ships.
//   * PropertyAnimationClipSource: the cooked wire, flat SoA parallel arrays as AnimationClipSource
//     uses, so the serializer never nests. FromClip and FillClip convert to and from the runtime clip.
//   * PropertyAnimationClipFactory: an IResourceFactory that reads the source object and fills a
//     clip resource.
//
// Reconstruction walks the same track-then-channel order the writer used, and FillClip bounds
// every index against the pools, so a malformed wire cannot overread.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.propertyanimation.resource;

import foundation.core;
import foundation.resource;
import foundation.content;
import foundation.propertyanimation;

using namespace foundation::core;

export namespace foundation::propertyanimation
{
    namespace resource = foundation::resource;

    // The runtime product: an evaluate-ready clip, RefCounted so the resource system and component Refs
    // hold it as shared immutable data.
    class PropertyAnimationClipResource final : public Object
    {
        RTTI_OBJECT(PropertyAnimationClipResource, Object)
    public:
        PropertyAnimationClip clip;
    };

    // The cooked wire. Flat parallel arrays; the scalar key pool is shared across all channels, indexed
    // per channel; channels appear in track-then-channel order; quat keys have their own pool indexed
    // per track.
    class PropertyAnimationClipSource final : public ISerializable
    {
        RTTI_OBJECT(PropertyAnimationClipSource, ISerializable)
    public:
        f32 duration = 0.0f;

        // Per track (N entries).
        Array<String> trackComponent;
        Array<String> trackPath;
        Array<u8> trackKind; // TrackValueKind

        // Per scalar channel (the sum over tracks of ChannelCount(kind), in track-then-channel order).
        Array<u32> channelKeyStart; // first key index into the key pool
        Array<u32> channelKeyCount;

        // Scalar key pool.
        Array<f32> keyTime;
        Array<f32> keyValue;
        Array<f32> keyTangentIn;
        Array<f32> keyTangentOut;
        Array<u8> keyInterp; // CurveKeyInterpolation
        Array<u8> keyTangentMode; // CurveTangentMode, what an editor keeps (appended: older wires have none)

        // Per track: quat key run (0 count for non-Quat tracks).
        Array<u32> trackQuatStart;
        Array<u32> trackQuatCount;
        Array<f32> quatTime;
        Array<Float4> quatValue; // xyzw

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "duration", duration);
            foundation::core::Serialize(ar, "trackComponent", trackComponent);
            foundation::core::Serialize(ar, "trackPath", trackPath);
            foundation::core::Serialize(ar, "trackKind", trackKind);
            foundation::core::Serialize(ar, "channelKeyStart", channelKeyStart);
            foundation::core::Serialize(ar, "channelKeyCount", channelKeyCount);
            foundation::core::Serialize(ar, "keyTime", keyTime);
            foundation::core::Serialize(ar, "keyValue", keyValue);
            foundation::core::Serialize(ar, "keyTangentIn", keyTangentIn);
            foundation::core::Serialize(ar, "keyTangentOut", keyTangentOut);
            foundation::core::Serialize(ar, "keyInterp", keyInterp);
            foundation::core::Serialize(ar, "trackQuatStart", trackQuatStart);
            foundation::core::Serialize(ar, "trackQuatCount", trackQuatCount);
            foundation::core::Serialize(ar, "quatTime", quatTime);
            foundation::core::Serialize(ar, "quatValue", quatValue);
            foundation::core::SerializeAppended(ar, "keyTangentMode", keyTangentMode);
        }

        // Flatten a runtime clip into the wire (cook side). Channels are appended in track-then-channel
        // order so FillClip's cursor walk reconstructs them identically.
        static void FromClip(const PropertyAnimationClip& clip, PropertyAnimationClipSource& out)
        {
            out.duration = clip.duration;
            out.trackComponent.Clear();
            out.trackPath.Clear();
            out.trackKind.Clear();
            out.channelKeyStart.Clear();
            out.channelKeyCount.Clear();
            out.keyTime.Clear();
            out.keyValue.Clear();
            out.keyTangentIn.Clear();
            out.keyTangentOut.Clear();
            out.keyInterp.Clear();
            out.keyTangentMode.Clear();
            out.trackQuatStart.Clear();
            out.trackQuatCount.Clear();
            out.quatTime.Clear();
            out.quatValue.Clear();

            for (const PropertyTrack& t : clip.tracks)
            {
                out.trackComponent.PushBack(String(t.componentType.AsView()));
                out.trackPath.PushBack(String(t.propertyPath.AsView()));
                out.trackKind.PushBack(static_cast<u8>(t.kind));

                const u32 channelCount = ChannelCount(t.kind);
                for (u32 c = 0; c < channelCount; ++c)
                {
                    const Array<CurveKey>& keys = t.channels[c].Keys();
                    out.channelKeyStart.PushBack(static_cast<u32>(out.keyTime.Size()));
                    out.channelKeyCount.PushBack(static_cast<u32>(keys.Size()));
                    for (const CurveKey& k : keys)
                    {
                        out.keyTime.PushBack(k.time);
                        out.keyValue.PushBack(k.value);
                        out.keyTangentIn.PushBack(k.tangentIn);
                        out.keyTangentOut.PushBack(k.tangentOut);
                        out.keyInterp.PushBack(static_cast<u8>(k.interpolation));
                        out.keyTangentMode.PushBack(static_cast<u8>(k.tangentMode));
                    }
                }

                out.trackQuatStart.PushBack(static_cast<u32>(out.quatTime.Size()));
                out.trackQuatCount.PushBack(static_cast<u32>(t.quatKeys.Size()));
                for (const QuatKey& q : t.quatKeys)
                {
                    out.quatTime.PushBack(q.time);
                    out.quatValue.PushBack(Float4{q.value.x, q.value.y, q.value.z, q.value.w});
                }
            }
        }

        // Rebuild a runtime clip from the wire (load side). Bounds every pool index; a malformed wire
        // yields a partial-but-safe clip rather than an overread.
        void FillClip(PropertyAnimationClip& out) const
        {
            out.duration = duration;
            out.tracks.Clear();

            const usize trackCount = trackKind.Size();
            usize channelCursor = 0;
            for (usize i = 0; i < trackCount; ++i)
            {
                PropertyTrack t;
                t.componentType =
                    (i < trackComponent.Size()) ? String(trackComponent[i].AsView()) : String();
                t.propertyPath = (i < trackPath.Size()) ? String(trackPath[i].AsView()) : String();
                // Range-guard the kind: an out-of-range byte would make ChannelCount desync every
                // later track's channel cursor. A corrupt kind reads as Float (a deterministic
                // one-channel walk).
                const u8 rawKind = trackKind[i];
                t.kind = (rawKind <= static_cast<u8>(TrackValueKind::Color))
                             ? static_cast<TrackValueKind>(rawKind)
                             : TrackValueKind::Float;

                const u32 channelCount = ChannelCount(t.kind);
                for (u32 c = 0; c < channelCount && channelCursor < channelKeyStart.Size(); ++c)
                {
                    const u32 start = channelKeyStart[channelCursor];
                    const u32 count =
                        (channelCursor < channelKeyCount.Size()) ? channelKeyCount[channelCursor] : 0;
                    for (u32 j = 0; j < count; ++j)
                    {
                        const usize idx = start + j;
                        if (idx >= keyTime.Size())
                        {
                            break;
                        }
                        CurveKey k;
                        k.time = keyTime[idx];
                        k.value = (idx < keyValue.Size()) ? keyValue[idx] : 0.0f;
                        k.tangentIn = (idx < keyTangentIn.Size()) ? keyTangentIn[idx] : 0.0f;
                        k.tangentOut = (idx < keyTangentOut.Size()) ? keyTangentOut[idx] : 0.0f;
                        // A truncated or out-of-range interp keeps the CurveKey default (Linear),
                        // rather than silently becoming Constant (0).
                        if (idx < keyInterp.Size() &&
                            keyInterp[idx] <= static_cast<u8>(CurveKeyInterpolation::Cubic))
                        {
                            k.interpolation = static_cast<CurveKeyInterpolation>(keyInterp[idx]);
                        }
                        // A wire without modes, or a mode out of range: Broken, the tangents as stored.
                        if (idx < keyTangentMode.Size() && keyTangentMode[idx] <= static_cast<u8>(CurveTangentMode::Flat))
                        {
                            k.tangentMode = static_cast<CurveTangentMode>(keyTangentMode[idx]);
                        }
                        if (c < kMaxChannels)
                        {
                            t.channels[c].AddKey(k);
                        }
                    }
                    ++channelCursor;
                }

                if (t.kind == TrackValueKind::Quat && i < trackQuatStart.Size())
                {
                    const u32 qStart = trackQuatStart[i];
                    const u32 qCount = (i < trackQuatCount.Size()) ? trackQuatCount[i] : 0;
                    for (u32 j = 0; j < qCount; ++j)
                    {
                        const usize idx = qStart + j;
                        if (idx >= quatTime.Size() || idx >= quatValue.Size())
                        {
                            break;
                        }
                        const Float4 v = quatValue[idx];
                        t.quatKeys.PushBack(QuatKey{quatTime[idx], Quaternion{v.x, v.y, v.z, v.w}});
                    }
                }

                out.tracks.PushBack(Move(t));
            }
        }
    };

    // Loads a cooked PropertyAnimationClipSource into a runtime PropertyAnimationClipResource.
    class PropertyAnimationClipFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &PropertyAnimationClipResource::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &PropertyAnimationClipSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            (void)manager;
            RefPtr<ISerializable> object = instance.ReadObject();
            PropertyAnimationClipSource* src = Cast<PropertyAnimationClipSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<PropertyAnimationClipResource> res =
                MakeRef<PropertyAnimationClipResource>(DefaultAllocator());
            src->FillClip(res->clip);
            return res;
        }
    };

    // Register the cooked source type for content-DB deserialization, and the runtime resource type.
    inline void RegisterPropertyAnimationResource()
    {
        GlobalTypeRegistry().Register(PropertyAnimationClipResource::StaticType());
        GlobalTypeRegistry().Register(PropertyAnimationClipSource::StaticType());
        RegisterSerializable<PropertyAnimationClipSource>();
    }

    // Runtime domain (these ship in the player, unlike authoring assets).
    RTTI_DEFINE_OBJECT(PropertyAnimationClipResource, "rtti::propertyanimation")
    RTTI_DEFINE_OBJECT(PropertyAnimationClipSource, "rtti::propertyanimation")
}

export namespace foundation::propertyanimation
{
    /// The property animation resource module: the module the engine composition composes this
    /// library's factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kPropertyAnimationResourceFactories[] = {
        foundation::resource::FactoryByDefault<PropertyAnimationClipResource, PropertyAnimationClipSource, PropertyAnimationClipFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kPropertyAnimationResourceModule{
        u8"propertyanimation", &RegisterPropertyAnimationResource, kPropertyAnimationResourceFactories,
        sizeof(kPropertyAnimationResourceFactories) / sizeof(kPropertyAnimationResourceFactories[0])};
}
