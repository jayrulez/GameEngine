/// Draconic::Animation - the `:clip` partition.
///
/// Animation data: keyframed tracks (position/rotation/scale per bone) + timed events, grouped into
/// an AnimationClip. Ported faithfully from Sedulous.Animation.AnimationClip / AnimationTrack /
/// Keyframe / AnimationEvent. Tracks are heap-owned (UniquePtr) so GetOrCreate*Track pointers stay
/// valid as more tracks are added (matching the Beef List<AnimationTrack<T>> of references).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.animation:clip;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::animation
{

    // Event callback: (event name, event time in seconds). The engine's move-only delegate. Defined here
    // (with AnimationEvent) so both the player and the graph's IAnimationStateNode can use it.
    using AnimationEventHandler = foundation::Function<void(StringView, f32)>;

    // Keyframe interpolation mode.
    enum class InterpolationMode
    {
        Step,
        Linear,
        CubicSpline
    };

    // A keyframed value at a time (tangents used only for cubic spline).
    template <typename T>
    struct Keyframe
    {
        f32 time = 0.0f;
        T value = {};
        T inTangent = {};
        T outTangent = {};

        Keyframe() = default;
        Keyframe(f32 t, const T& v) : time(t), value(v) {}
        Keyframe(f32 t, const T& v, const T& in, const T& out)
            : time(t), value(v), inTangent(in), outTangent(out)
        {
        }
    };

    // The keyframe interval surrounding a sample time + the interpolation factor.
    struct KeyframeLookup
    {
        i32 prev = -1;
        i32 next = -1;
        f32 t = 0.0f;
    };

    // Keyframes for one property of one bone.
    template <typename T>
    class AnimationTrack
    {
    public:
        i32 boneIndex = 0;
        InterpolationMode interpolation = InterpolationMode::Linear;

        [[nodiscard]] Array<Keyframe<T>>& Keyframes() noexcept { return m_keyframes; }
        [[nodiscard]] const Array<Keyframe<T>>& Keyframes() const noexcept { return m_keyframes; }

        void AddKeyframe(f32 time, const T& value)
        {
            m_keyframes.PushBack(Keyframe<T>{time, value});
        }
        void AddKeyframe(f32 time, const T& value, const T& inTan, const T& outTan)
        {
            m_keyframes.PushBack(Keyframe<T>{time, value, inTan, outTan});
        }

        // Stable insertion sort by time (keyframes are typically already time-ordered from import).
        void SortKeyframes()
        {
            for (usize i = 1; i < m_keyframes.Size(); ++i)
            {
                Keyframe<T> key = m_keyframes[i];
                usize j = i;
                while (j > 0 && m_keyframes[j - 1].time > key.time)
                {
                    m_keyframes[j] = m_keyframes[j - 1];
                    --j;
                }
                m_keyframes[j] = key;
            }
        }

        // Find the keyframe interval (prev,next) + factor t for `time`. Clamps to ends.
        [[nodiscard]] KeyframeLookup FindKeyframes(f32 time) const
        {
            const i32 count = static_cast<i32>(m_keyframes.Size());
            if (count == 0)
            {
                return KeyframeLookup{-1, -1, 0.0f};
            }
            if (count == 1)
            {
                return KeyframeLookup{0, 0, 0.0f};
            }
            if (time <= m_keyframes[0].time)
            {
                return KeyframeLookup{0, 0, 0.0f};
            }
            if (time >= m_keyframes[static_cast<usize>(count - 1)].time)
            {
                return KeyframeLookup{count - 1, count - 1, 0.0f};
            }

            i32 low = 0, high = count - 1;
            while (low < high - 1)
            {
                const i32 mid = (low + high) / 2;
                if (m_keyframes[static_cast<usize>(mid)].time <= time)
                {
                    low = mid;
                }
                else
                {
                    high = mid;
                }
            }
            const f32 duration = m_keyframes[static_cast<usize>(high)].time -
                                 m_keyframes[static_cast<usize>(low)].time;
            const f32 t = (duration > 0.0f)
                              ? (time - m_keyframes[static_cast<usize>(low)].time) / duration
                              : 0.0f;
            return KeyframeLookup{low, high, t};
        }

    private:
        Array<Keyframe<T>> m_keyframes;
    };

    // An event placed at a time in a clip; fires when playback crosses it.
    struct AnimationEvent
    {
        f32 time = 0.0f;
        String name;
        AnimationEvent() = default;
        AnimationEvent(f32 t, StringView n) : time(t), name(n) {}
    };

    // All tracks + events for one animation. A resource product (Object) so a cooked AnimationClipSource
    // can build into it via the resource system.
    class AnimationClip : public Object
    {
        DRACONIC_OBJECT(AnimationClip, Object)
    public:
        AnimationClip() = default;
        explicit AnimationClip(StringView name, f32 duration = 0.0f, bool isLooping = false)
            : duration(duration), isLooping(isLooping), m_name(name)
        {
        }

        AnimationClip(const AnimationClip&) = delete;
        AnimationClip& operator=(const AnimationClip&) = delete;

        f32 duration = 0.0f;
        bool isLooping = false;

        [[nodiscard]] String& Name() noexcept { return m_name; }
        [[nodiscard]] const String& Name() const noexcept { return m_name; }

        using Vec3Track = AnimationTrack<Float3>;
        using QuatTrack = AnimationTrack<Quaternion>;

        [[nodiscard]] Array<UniquePtr<Vec3Track>>& PositionTracks() noexcept
        {
            return m_positionTracks;
        }
        [[nodiscard]] Array<UniquePtr<QuatTrack>>& RotationTracks() noexcept
        {
            return m_rotationTracks;
        }
        [[nodiscard]] Array<UniquePtr<Vec3Track>>& ScaleTracks() noexcept { return m_scaleTracks; }
        [[nodiscard]] Array<AnimationEvent>& Events() noexcept { return m_events; }
        [[nodiscard]] const Array<UniquePtr<Vec3Track>>& PositionTracks() const noexcept
        {
            return m_positionTracks;
        }
        [[nodiscard]] const Array<UniquePtr<QuatTrack>>& RotationTracks() const noexcept
        {
            return m_rotationTracks;
        }
        [[nodiscard]] const Array<UniquePtr<Vec3Track>>& ScaleTracks() const noexcept
        {
            return m_scaleTracks;
        }
        [[nodiscard]] const Array<AnimationEvent>& Events() const noexcept { return m_events; }

        [[nodiscard]] Vec3Track* GetOrCreatePositionTrack(i32 boneIndex)
        {
            return GetOrCreate(m_positionTracks, boneIndex);
        }
        [[nodiscard]] QuatTrack* GetOrCreateRotationTrack(i32 boneIndex)
        {
            return GetOrCreate(m_rotationTracks, boneIndex);
        }
        [[nodiscard]] Vec3Track* GetOrCreateScaleTrack(i32 boneIndex)
        {
            return GetOrCreate(m_scaleTracks, boneIndex);
        }

        void SortAllKeyframes()
        {
            for (auto& t : m_positionTracks)
            {
                t->SortKeyframes();
            }
            for (auto& t : m_rotationTracks)
            {
                t->SortKeyframes();
            }
            for (auto& t : m_scaleTracks)
            {
                t->SortKeyframes();
            }
        }

        void AddEvent(f32 time, StringView name) { m_events.PushBack(AnimationEvent{time, name}); }

        void SortEvents()
        {
            for (usize i = 1; i < m_events.Size(); ++i)
            {
                AnimationEvent key = static_cast<AnimationEvent&&>(m_events[i]);
                usize j = i;
                while (j > 0 && m_events[j - 1].time > key.time)
                {
                    m_events[j] = static_cast<AnimationEvent&&>(m_events[j - 1]);
                    --j;
                }
                m_events[j] = static_cast<AnimationEvent&&>(key);
            }
        }

        // Fire events crossed in (prevTime, currentTime]. currentTime may exceed Duration when looping.
        // `handler` is any callable (StringView name, f32 time). Faithful to Sedulous FireEvents.
        template <typename Handler>
        void FireEvents(f32 prevTime, f32 currentTime, Handler&& handler) const
        {
            if (m_events.IsEmpty() || duration <= 0.0f)
            {
                return;
            }

            if (currentTime > prevTime && currentTime <= duration)
            {
                for (const AnimationEvent& e : m_events)
                {
                    if (e.time > prevTime && e.time <= currentTime)
                    {
                        handler(StringView{e.name}, e.time);
                    }
                }
            }
            else if (currentTime > duration)
            {
                if (isLooping)
                {
                    for (const AnimationEvent& e : m_events)
                    {
                        if (e.time > prevTime && e.time <= duration)
                        {
                            handler(StringView{e.name}, e.time);
                        }
                    }
                    f32 wrapped = currentTime;
                    while (wrapped >= duration)
                    {
                        wrapped -= duration;
                    }
                    for (const AnimationEvent& e : m_events)
                    {
                        if (e.time <= wrapped)
                        {
                            handler(StringView{e.name}, e.time);
                        }
                    }
                }
                else
                {
                    for (const AnimationEvent& e : m_events)
                    {
                        if (e.time > prevTime && e.time <= duration)
                        {
                            handler(StringView{e.name}, e.time);
                        }
                    }
                }
            }
        }

        // Duration = latest keyframe time across all tracks.
        void ComputeDuration()
        {
            duration = 0.0f;
            for (auto& t : m_positionTracks)
            {
                if (!t->Keyframes().IsEmpty())
                {
                    duration = Max(duration, t->Keyframes()[t->Keyframes().Size() - 1].time);
                }
            }
            for (auto& t : m_rotationTracks)
            {
                if (!t->Keyframes().IsEmpty())
                {
                    duration = Max(duration, t->Keyframes()[t->Keyframes().Size() - 1].time);
                }
            }
            for (auto& t : m_scaleTracks)
            {
                if (!t->Keyframes().IsEmpty())
                {
                    duration = Max(duration, t->Keyframes()[t->Keyframes().Size() - 1].time);
                }
            }
        }

        // Reset in place (resource hot-reload keeps outside references valid).
        void ClearForReload()
        {
            m_positionTracks.Clear();
            m_rotationTracks.Clear();
            m_scaleTracks.Clear();
            m_events.Clear();
            duration = 0.0f;
            isLooping = false;
            m_name.Clear();
        }

    private:
        template <typename Track>
        [[nodiscard]] Track* GetOrCreate(Array<UniquePtr<Track>>& tracks, i32 boneIndex)
        {
            for (auto& t : tracks)
            {
                if (t->boneIndex == boneIndex)
                {
                    return t.Get();
                }
            }
            UniquePtr<Track> track = MakeUnique<Track>(DefaultAllocator());
            track->boneIndex = boneIndex;
            Track* raw = track.Get();
            tracks.PushBack(static_cast<UniquePtr<Track>&&>(track));
            return raw;
        }

        String m_name;
        Array<UniquePtr<Vec3Track>> m_positionTracks;
        Array<UniquePtr<QuatTrack>> m_rotationTracks;
        Array<UniquePtr<Vec3Track>> m_scaleTracks;
        Array<AnimationEvent> m_events;
    };

    DRACONIC_DEFINE_OBJECT(AnimationClip, "draconic::animation")

} // namespace draconic::animation
