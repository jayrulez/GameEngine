/// Draconic::Animation - the `:player` partition.
///
/// AnimationPlayer: single-clip playback for one skeleton instance - time advance, looping/clamping,
/// event firing, and evaluation into skinning matrices (+ previous frame for motion vectors). Ported
/// faithfully from Sedulous.Animation.AnimationPlayer. The skeleton + clip are borrowed (not owned).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.animation:player;

import draconic.foundation;
import :skeleton;
import :clip;
import :sampler;
import :pose;

using namespace draconic::foundation;

export namespace draconic::animation
{

    enum class PlaybackState
    {
        Stopped,
        Playing,
        Paused
    };
    // AnimationEventHandler is defined in :clip (shared with the graph).

    class AnimationPlayer
    {
    public:
        explicit AnimationPlayer(Skeleton& skeleton) : m_skeleton(&skeleton)
        {
            const usize boneCount = static_cast<usize>(skeleton.BoneCount());
            m_localPoses.Resize(boneCount);
            m_skinningMatrices.Resize(boneCount);
            m_prevSkinningMatrices.Resize(boneCount);
            ResetToBind();
        }

        ~AnimationPlayer() = default;
        AnimationPlayer(const AnimationPlayer&) = delete;
        AnimationPlayer& operator=(const AnimationPlayer&) = delete;

        [[nodiscard]] Skeleton& GetSkeleton() noexcept { return *m_skeleton; }
        [[nodiscard]] AnimationClip* CurrentClip() const noexcept { return m_currentClip; }
        [[nodiscard]] PlaybackState State() const noexcept { return m_state; }
        [[nodiscard]] f32 CurrentTime() const noexcept { return m_currentTime; }

        f32 speed = 1.0f; // playback speed multiplier

        // Setting the time marks the skinning matrices dirty (re-evaluated on next access).
        void SetCurrentTime(f32 t) noexcept
        {
            if (m_currentTime != t)
            {
                m_currentTime = t;
                m_matricesDirty = true;
            }
        }

        // Plays a clip (borrowed). restart resets the clock to 0.
        void Play(AnimationClip* clip, bool restart = true)
        {
            m_currentClip = clip;
            if (restart)
            {
                m_currentTime = 0.0f;
                m_prevTime = 0.0f;
            }
            m_state = PlaybackState::Playing;
            m_matricesDirty = true;
        }

        void Stop()
        {
            m_state = PlaybackState::Stopped;
            m_currentTime = 0.0f;
            m_prevTime = 0.0f;
            m_currentClip = nullptr;
            ResetToBind();
        }

        void Pause()
        {
            if (m_state == PlaybackState::Playing)
            {
                m_state = PlaybackState::Paused;
            }
        }
        void Resume()
        {
            if (m_state == PlaybackState::Paused)
            {
                m_state = PlaybackState::Playing;
            }
        }

        // Sets the event handler (takes ownership; replaces any previous handler).
        void SetEventHandler(AnimationEventHandler handler)
        {
            m_eventHandler = static_cast<AnimationEventHandler&&>(handler);
        }

        // Resets all local poses to the skeleton's bind pose.
        void ResetToBind()
        {
            for (i32 i = 0;
                 i < m_skeleton->BoneCount() && static_cast<usize>(i) < m_localPoses.Size(); ++i)
            {
                const Bone* bone = m_skeleton->GetBone(i);
                m_localPoses[static_cast<usize>(i)] =
                    (bone != nullptr) ? bone->localBindPose : BoneTransform{};
            }
            m_matricesDirty = true;
        }

        // Advances playback by deltaTime: fires crossed events, then loops/clamps.
        void Update(f32 deltaTime)
        {
            if (m_state != PlaybackState::Playing || m_currentClip == nullptr)
            {
                return;
            }

            // Snapshot current skinning matrices as the previous frame (motion vectors).
            const usize n = m_skinningMatrices.Size();
            for (usize i = 0; i < n; ++i)
            {
                m_prevSkinningMatrices[i] = m_skinningMatrices[i];
            }

            const f32 prevTime = m_prevTime;
            m_currentTime += deltaTime * speed;

            // Fire events before wrapping (so loop crossings are detected).
            if (m_eventHandler && !m_currentClip->Events().IsEmpty())
            {
                m_currentClip->FireEvents(prevTime, m_currentTime, [this](StringView name, f32 time)
                                          { m_eventHandler(name, time); });
            }

            if (m_currentClip->isLooping)
            {
                if (m_currentClip->duration > 0.0f)
                {
                    while (m_currentTime >= m_currentClip->duration)
                    {
                        m_currentTime -= m_currentClip->duration;
                    }
                    while (m_currentTime < 0.0f)
                    {
                        m_currentTime += m_currentClip->duration;
                    }
                }
            }
            else
            {
                if (m_currentTime >= m_currentClip->duration)
                {
                    m_currentTime = m_currentClip->duration;
                    m_state = PlaybackState::Stopped;
                }
                else if (m_currentTime < 0.0f)
                {
                    m_currentTime = 0.0f;
                    m_state = PlaybackState::Stopped;
                }
            }

            m_prevTime = m_currentTime;
            m_matricesDirty = true;
        }

        // Samples the current clip + computes skinning matrices (only if dirty). Call before rendering.
        void Evaluate()
        {
            if (!m_matricesDirty)
            {
                return;
            }
            if (m_currentClip != nullptr)
            {
                SampleClip(*m_currentClip, *m_skeleton, m_currentTime,
                           Span<BoneTransform>{m_localPoses.Data(), m_localPoses.Size()});
            }
            m_skeleton->ComputeSkinningMatrices(
                Span<const BoneTransform>{m_localPoses.Data(), m_localPoses.Size()},
                Span<Float4x4>{m_skinningMatrices.Data(), m_skinningMatrices.Size()});
            m_matricesDirty = false;
        }

        // Current skinning matrices for GPU upload (evaluates if needed).
        [[nodiscard]] Span<const Float4x4> GetSkinningMatrices()
        {
            Evaluate();
            return Span<const Float4x4>{m_skinningMatrices.Data(), m_skinningMatrices.Size()};
        }
        [[nodiscard]] Span<const Float4x4> GetPrevSkinningMatrices() const
        {
            return Span<const Float4x4>{m_prevSkinningMatrices.Data(),
                                        m_prevSkinningMatrices.Size()};
        }

        // Push externally-computed matrices (used by the graph player to drive this player's output).
        void OverrideSkinningMatrices(Span<const Float4x4> current, Span<const Float4x4> prev)
        {
            const usize c = Min(current.Size(), m_skinningMatrices.Size());
            for (usize i = 0; i < c; ++i)
            {
                m_skinningMatrices[i] = current[i];
            }
            const usize p = Min(prev.Size(), m_prevSkinningMatrices.Size());
            for (usize i = 0; i < p; ++i)
            {
                m_prevSkinningMatrices[i] = prev[i];
            }
            m_matricesDirty = false;
        }

        [[nodiscard]] Span<BoneTransform> GetLocalPoses() noexcept
        {
            return {m_localPoses.Data(), m_localPoses.Size()};
        }
        [[nodiscard]] AnimationPose GetPose() noexcept { return AnimationPose{GetLocalPoses()}; }

        // Directly set a bone's local transform (procedural animation).
        void SetBonePose(i32 boneIndex, const BoneTransform& pose)
        {
            if (boneIndex >= 0 && static_cast<usize>(boneIndex) < m_localPoses.Size())
            {
                m_localPoses[static_cast<usize>(boneIndex)] = pose;
                m_matricesDirty = true;
            }
        }

        // Blend another clip on top of the current local poses (weight 0..1).
        void BlendAnimation(AnimationClip* clip, f32 time, f32 weight)
        {
            if (clip == nullptr || weight <= 0.0f)
            {
                return;
            }
            Array<BoneTransform> blend;
            blend.Resize(static_cast<usize>(m_skeleton->BoneCount()));
            SampleClip(*clip, *m_skeleton, time, Span<BoneTransform>{blend.Data(), blend.Size()});
            const usize n = Min(m_localPoses.Size(), blend.Size());
            for (usize i = 0; i < n; ++i)
            {
                m_localPoses[i] = BoneTransform::Lerp(m_localPoses[i], blend[i], weight);
            }
            m_matricesDirty = true;
        }

    private:
        Skeleton* m_skeleton = nullptr;         // borrowed
        AnimationClip* m_currentClip = nullptr; // borrowed
        f32 m_currentTime = 0.0f;
        f32 m_prevTime = 0.0f;
        PlaybackState m_state = PlaybackState::Stopped;
        bool m_matricesDirty = true;

        Array<BoneTransform> m_localPoses;
        Array<Float4x4> m_skinningMatrices;
        Array<Float4x4> m_prevSkinningMatrices;
        AnimationEventHandler m_eventHandler;
    };

} // namespace draconic::animation
